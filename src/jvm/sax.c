/*
 * v36.33 [SAX-XML]: minimal javax.xml.parsers (SAX) support on top of a
 * small built-in XML scanner.
 *
 * Field case: Speedx 3D (com.gamelion.speedx.BitmapFont) parses its bitmap
 * font descriptors (font_*.xml, BMFont format) with the standard SAX flow:
 *   SAXParserFactory.newInstance().newSAXParser().parse(in, handler)
 * NOJME had no org.xml.sax / javax.xml.parsers classes at all, so
 * newInstance() resolved to an unimplemented-native default (null) and the
 * very next virtual call died with
 *   NPE "Null receiver calling javax/xml/parsers/SAXParserFactory..."
 * — the game booted with no text rendered anywhere.
 *
 * Implemented surface (enough for BMFont-style descriptors and simple
 * documents):
 *   javax/xml/parsers/SAXParserFactory : newInstance, newSAXParser,
 *                                        setValidating/setNamespaceAware/
 *                                        setCoalescing/setExpandEntity-
 *                                        References (no-ops), is*
 *   javax/xml/parsers/SAXParser        : parse(InputStream,DefaultHandler)
 *                                        parse(InputStream,DefaultHandler,
 *                                        String), reset, getProperty/
 *                                        setProperty no-ops
 *   org/xml/sax/helpers/DefaultHandler : no-op startDocument/endDocument/
 *                                        startElement/endElement/characters
 *                                        (games override what they need)
 *   org/xml/sax/helpers/AttributesImpl : getLength/getQName/getValue/
 *                                        getIndex/getURI/getLocalName/
 *                                        getType backed by the CURRENT
 *                                        startElement event
 *   org/xml/sax/SAXException           : exists as a stub exception class
 * The scanner understands elements, attributes (single/double quotes),
 * entity references (&amp; &lt; &gt; &quot; &apos; &#NN; &#xHH;), comments,
 * CDATA sections, processing instructions and DOCTYPE skip. End tags are
 * matched leniently against an element stack (misnested input never kills
 * the game thread — parse() throws SAXException on hard errors only).
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <stdint.h>

#include "debug.h"
#include "debug_macros.h"
#include "jvm.h"
#include "native.h"
#include "heap.h"
#include "opcodes.h"

#ifndef SAX_DEBUG
#define SAX_DEBUG(fmt, ...)    MODULE_LOG_TS("[SAX]", fmt, ##__VA_ARGS__)
#endif

/* ======================================================================
 * Current-event attributes (the AttributesImpl peer)
 * ====================================================================== */

#define SAX_MAX_ATTRS  64
#define SAX_ATTR_CAP   512

typedef struct {
    char  qname[SAX_MAX_ATTRS][128];
    char  value[SAX_MAX_ATTRS][SAX_ATTR_CAP];
    int   count;
} SaxAttrs;

static SaxAttrs g_sax_attrs;
static JavaObject* g_sax_attrs_obj = NULL;   /* AttributesImpl handed to callbacks */

static void sax_attrs_clear(void) {
    g_sax_attrs.count = 0;
}

static void sax_attrs_add(const char* qname, const char* value) {
    if (g_sax_attrs.count >= SAX_MAX_ATTRS) return;
    int i = g_sax_attrs.count++;
    snprintf(g_sax_attrs.qname[i], sizeof(g_sax_attrs.qname[0]), "%s", qname);
    snprintf(g_sax_attrs.value[i], sizeof(g_sax_attrs.value[0]), "%s",
             value ? value : "");
}

static int sax_attrs_index(const char* qname) {
    for (int i = 0; i < g_sax_attrs.count; i++) {
        if (strcmp(g_sax_attrs.qname[i], qname) == 0) return i;
    }
    return -1;
}

/* ======================================================================
 * Callback dispatch (walk the handler's class hierarchy, then execute)
 * ====================================================================== */

static JavaMethod* sax_resolve(JavaObject* handler, const char* name,
                               const char* desc) {
    JavaClass* c = handler ? handler->header.clazz : NULL;
    while (c) {
        for (int i = 0; i < c->methods_count; i++) {
            JavaMethod* m = &c->methods[i];
            if (m->name && strcmp(m->name, name) == 0 &&
                m->descriptor && strcmp(m->descriptor, desc) == 0) {
                return m;
            }
        }
        c = c->super_class;
    }
    return NULL;
}

/* Invoke a handler callback. Returns false when the callback threw (the
 * exception is then wrapped into SAXException so the game's catch works). */
static bool sax_invoke(JVM* jvm, JavaThread* thread, JavaObject* handler,
                       const char* name, const char* desc,
                       JavaValue* args, int arg_count) {
    (void)arg_count;
    extern int execute_method(JVM* jvm, JavaThread* thread, JavaMethod* method,
                              JavaValue* args, JavaValue* result);
    JavaMethod* m = sax_resolve(handler, name, desc);
    if (m) {
        JavaValue result;
        memset(&result, 0, sizeof(result));
        execute_method(jvm, thread, m, args, &result);
        if (thread->pending_exception) {
            thread->pending_exception = NULL;
            jvm_throw_by_name(jvm, "org/xml/sax/SAXException", name);
            return false;
        }
    }
    /* Unresolved callbacks are simply not delivered (DefaultHandler no-ops
     * are registered as natives; a game overriding them wins). */
    return true;
}

static bool sax_characters(JVM* jvm, JavaThread* thread, JavaObject* handler,
                           const char* utf8, int n) {
    if (n <= 0) return true;
    JavaArray* arr = jvm_new_array(jvm, T_CHAR, n, NULL);
    if (!arr) return true;  /* OOM mid-document: silently drop the text run */
    jchar* d = (jchar*)array_data(arr);
    for (int i = 0; i < n; i++) d[i] = (jchar)(unsigned char)utf8[i];
    JavaValue args[4];
    args[0].ref = handler;
    args[1].ref = (JavaObject*)arr;
    args[2].i = 0;
    args[3].i = n;
    return sax_invoke(jvm, thread, handler, "characters", "([CII)V", args, 4);
}

static bool sax_start_element(JVM* jvm, JavaThread* thread, JavaObject* handler,
                              JavaObject* attrs_obj, const char* name) {
    JavaString* ns   = jvm_new_string(jvm, "");
    JavaString* ln   = jvm_new_string(jvm, name);
    JavaString* qn   = jvm_new_string(jvm, name);
    if (!ns || !ln || !qn) return true;
    JavaValue args[5];
    args[0].ref = handler;
    args[1].ref = (JavaObject*)ns;   /* uri */
    args[2].ref = (JavaObject*)ln;   /* localName */
    args[3].ref = (JavaObject*)qn;   /* qName */
    args[4].ref = attrs_obj;         /* attributes */
    return sax_invoke(jvm, thread, handler, "startElement",
                      "(Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;"
                      "Lorg/xml/sax/Attributes;)V", args, 5);
}

static bool sax_end_element(JVM* jvm, JavaThread* thread, JavaObject* handler,
                            const char* name) {
    JavaString* ns = jvm_new_string(jvm, "");
    JavaString* ln = jvm_new_string(jvm, name);
    JavaString* qn = jvm_new_string(jvm, name);
    if (!ns || !ln || !qn) return true;
    JavaValue args[4];
    args[0].ref = handler;
    args[1].ref = (JavaObject*)ns;
    args[2].ref = (JavaObject*)ln;
    args[3].ref = (JavaObject*)qn;
    return sax_invoke(jvm, thread, handler, "endElement",
                      "(Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;)V",
                      args, 4);
}

static bool sax_simple_event(JVM* jvm, JavaThread* thread, JavaObject* handler,
                             const char* name) {
    JavaValue args[1];
    args[0].ref = handler;
    return sax_invoke(jvm, thread, handler, name, "()V", args, 1);
}

/* ======================================================================
 * Entity decoding + the XML scanner
 * ====================================================================== */

/* Decode entity references in-place-ish into out; returns chars written. */
static int sax_decode_entities(const char* src, int n, char* out, int out_cap) {
    int o = 0;
    for (int i = 0; i < n && o < out_cap - 8; ) {
        if (src[i] == '&') {
            int j = i + 1;
            while (j < n && src[j] != ';' && (j - i) < 12) j++;
            if (j < n && src[j] == ';' && (j - i) >= 3) {
                const char* e = src + i + 1;
                int el = (int)(j - i - 1);
                if (el == 4 && strncmp(e, "amp", 4) == 0)      { out[o++] = '&'; i = j + 1; continue; }
                if (el == 3 && strncmp(e, "lt", 3) == 0)        { out[o++] = '<'; i = j + 1; continue; }
                if (el == 3 && strncmp(e, "gt", 3) == 0)        { out[o++] = '>'; i = j + 1; continue; }
                if (el == 4 && strncmp(e, "quot", 4) == 0)      { out[o++] = '"'; i = j + 1; continue; }
                if (el == 4 && strncmp(e, "apos", 4) == 0)      { out[o++] = '\''; i = j + 1; continue; }
                if (e[0] == '#') {
                    int cp = 0;
                    bool ok = false;
                    if (el >= 3 && (e[1] == 'x' || e[1] == 'X')) {
                        cp = 0; ok = true;
                        for (int k = 2; k < el; k++) {
                            char h = e[k];
                            int dv = (h >= '0' && h <= '9') ? h - '0' :
                                     (h >= 'a' && h <= 'f') ? h - 'a' + 10 :
                                     (h >= 'A' && h <= 'F') ? h - 'A' + 10 : -1;
                            if (dv < 0) { ok = false; break; }
                            cp = cp * 16 + dv;
                        }
                    } else {
                        cp = 0; ok = el > 2;
                        for (int k = 1; k < el && ok; k++) {
                            if (e[k] < '0' || e[k] > '9') { ok = false; break; }
                            cp = cp * 10 + (e[k] - '0');
                        }
                    }
                    if (ok && cp > 0 && cp < 0x10000) {
                        out[o++] = (char)(cp < 128 ? cp : '?');  /* latin-1-ish */
                        i = j + 1;
                        continue;
                    }
                }
            }
        }
        out[o++] = src[i++];
    }
    out[o] = '\0';
    return o;
}

#define SAX_MAX_DEPTH 64

static bool sax_parse_buffer(JVM* jvm, JavaThread* thread, JavaObject* handler,
                             const char* data, int len) {
    if (!handler || !data || len <= 0) return true;

    if (!sax_simple_event(jvm, thread, handler, "startDocument")) return false;

    const char* stack[SAX_MAX_DEPTH];
    int depth = 0;
    int i = 0;
    bool ok = true;

    while (i < len && ok) {
        if (data[i] != '<') {
            int start = i;
            while (i < len && data[i] != '<') i++;
            char decoded[2048];
            int dn = sax_decode_entities(data + start, i - start,
                                         decoded, (int)sizeof(decoded));
            if (dn > 0) {
                ok = sax_characters(jvm, thread, handler, decoded, dn);
            }
            continue;
        }

        /* Markup */
        if (i + 3 < len && strncmp(data + i, "<!--", 4) == 0) {
            const char* end = strstr(data + i + 4, "-->");
            i = end ? (int)(end - data) + 3 : len;
            continue;
        }
        if (i + 8 < len && strncmp(data + i, "<![CDATA[", 9) == 0) {
            const char* end = strstr(data + i + 9, "]]>");
            int cend = end ? (int)(end - data) : len;
            char decoded[2048];
            int dn = sax_decode_entities(data + i + 9, cend - (i + 9),
                                         decoded, (int)sizeof(decoded));
            if (dn > 0) ok = sax_characters(jvm, thread, handler, decoded, dn);
            i = end ? cend + 3 : len;
            continue;
        }
        if (i + 1 < len && data[i + 1] == '?') {
            const char* end = strstr(data + i + 2, "?>");
            i = end ? (int)(end - data) + 2 : len;
            continue;
        }
        if (i + 1 < len && data[i + 1] == '!') {
            /* DOCTYPE and friends: skip to the next '>' (font descriptors
             * never carry internal subsets with nested brackets). */
            while (i < len && data[i] != '>') i++;
            i++;  /* past '>' */
            continue;
        }

        if (i + 1 < len && data[i + 1] == '/') {
            /* End tag */
            int s = i + 2;
            while (i < len && data[i] != '>') i++;
            int e = i; i++;  /* past '>' */
            while (e > s && (data[e-1] == ' ' || data[e-1] == '\t' ||
                             data[e-1] == '\r' || data[e-1] == '\n')) e--;
            char name[256];
            int nl = e - s;
            if (nl <= 0 || nl >= (int)sizeof(name)) continue;
            memcpy(name, data + s, (size_t)nl);
            name[nl] = '\0';

            /* Lenient stack matching: pop until the matching start tag. */
            int found = -1;
            for (int d = depth - 1; d >= 0; d--) {
                if (strcmp(stack[d], name) == 0) { found = d; break; }
            }
            if (found < 0) continue;  /* stray end tag: ignore */
            while (depth > found) {
                depth--;
                ok = sax_end_element(jvm, thread, handler, stack[depth]);
                if (!ok) break;
            }
            continue;
        }

        /* Start tag */
        {
            int s = i + 1;
            int j = s;
            while (j < len && (data[j] != '>' )) {
                if (data[j] == '<') break;  /* malformed: bail to next tag */
                j++;
            }
            if (j >= len) break;
            bool self_closing = (j > s && data[j - 1] == '/');
            int tag_end = j;
            if (self_closing) tag_end--;

            /* Tag name */
            int k = s;
            while (k < tag_end && (unsigned char)data[k] > ' ' &&
                   data[k] != '/' && data[k] != '>') k++;
            int nl = k - s;
            if (nl <= 0 || nl >= 256) { i = j + 1; continue; }
            char name[256];
            memcpy(name, data + s, (size_t)nl);
            name[nl] = '\0';

            /* Attributes */
            sax_attrs_clear();
            while (k < tag_end) {
                while (k < tag_end && ((unsigned char)data[k] <= ' ' ||
                                       data[k] == '/')) k++;
                if (k >= tag_end) break;
                int an = 0;
                char aname[128];
                while (k < tag_end && (unsigned char)data[k] > ' ' &&
                       data[k] != '=' && an < (int)sizeof(aname) - 1) {
                    aname[an++] = data[k++];
                }
                aname[an] = '\0';
                while (k < tag_end && ((unsigned char)data[k] <= ' ')) k++;
                if (k >= tag_end || data[k] != '=') {
                    if (an > 0) sax_attrs_add(aname, "");
                    continue;
                }
                k++;  /* past '=' */
                while (k < tag_end && ((unsigned char)data[k] <= ' ')) k++;
                if (k >= tag_end || (data[k] != '"' && data[k] != '\'')) {
                    if (an > 0) sax_attrs_add(aname, "");
                    continue;
                }
                char q = data[k++];
                int vs = k;
                while (k < tag_end && data[k] != q) k++;
                int vn = k - vs;
                if (k < tag_end) k++;  /* past closing quote */
                char decoded[SAX_ATTR_CAP];
                sax_decode_entities(data + vs, vn, decoded, (int)sizeof(decoded));
                if (an > 0) sax_attrs_add(aname, decoded);
            }

            ok = sax_start_element(jvm, thread, handler, g_sax_attrs_obj, name);
            if (!ok) break;

            if (!self_closing) {
                if (depth < SAX_MAX_DEPTH) {
                    stack[depth++] = strdup(name);
                } else {
                    /* Overly nested document: treat as self-closed */
                    ok = sax_end_element(jvm, thread, handler, name);
                    if (!ok) break;
                }
            }
            i = j + 1;
        }
    }

    /* Close anything left open (lenient) */
    while (depth > 0 && ok) {
        depth--;
        ok = sax_end_element(jvm, thread, handler, stack[depth]);
    }
    for (int d = 0; d < depth; d++) free((void*)stack[d]);

    if (ok) ok = sax_simple_event(jvm, thread, handler, "endDocument");
    return ok;
}

/* ======================================================================
 * SAXParser.parse(InputStream, DefaultHandler)
 * ====================================================================== */

/* v36.33: games wrap their stream in an InputSource
 * (new InputSource(getResourceAsStream(...)) then parse(source, handler)).
 * Stub InputSource objects carry no Java fields, so the wrapped stream is
 * remembered in this small ring (most-recent per object; parses are
 * synchronous, so a single live entry per parse would even suffice). */
#define SAX_MAX_INPUTSOURCES 32
static struct {
    JavaObject* src;
    JavaObject* stream;
} g_sax_inputsrc[SAX_MAX_INPUTSOURCES];
static int g_sax_inputsrc_n = 0;

static void sax_inputsource_remember(JavaObject* src, JavaObject* stream) {
    for (int i = 0; i < g_sax_inputsrc_n; i++) {
        if (g_sax_inputsrc[i].src == src) {
            g_sax_inputsrc[i].stream = stream;
            return;
        }
    }
    if (g_sax_inputsrc_n < SAX_MAX_INPUTSOURCES) {
        g_sax_inputsrc[g_sax_inputsrc_n].src = src;
        g_sax_inputsrc[g_sax_inputsrc_n].stream = stream;
        g_sax_inputsrc_n++;
        return;
    }
    /* Ring full: overwrite the oldest entry */
    memmove(&g_sax_inputsrc[0], &g_sax_inputsrc[1],
            sizeof(g_sax_inputsrc[0]) * (SAX_MAX_INPUTSOURCES - 1));
    g_sax_inputsrc[SAX_MAX_INPUTSOURCES - 1].src = src;
    g_sax_inputsrc[SAX_MAX_INPUTSOURCES - 1].stream = stream;
}

static JavaObject* sax_inputsource_stream(JavaObject* src) {
    for (int i = g_sax_inputsrc_n - 1; i >= 0; i--) {
        if (g_sax_inputsrc[i].src == src) return g_sax_inputsrc[i].stream;
    }
    return NULL;
}

static JavaValue native_inputsource_init_stream(JVM* jvm, JavaThread* thread,
                                                JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    JavaObject* self = (JavaObject*)args[0].ref;
    JavaObject* stream = (JavaObject*)args[1].ref;
    if (self) sax_inputsource_remember(self, stream);
    return NATIVE_RETURN_VOID();
}

static JavaValue native_inputsource_init_nostream(JVM* jvm, JavaThread* thread,
                                                  JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    JavaObject* self = (JavaObject*)args[0].ref;
    if (self) sax_inputsource_remember(self, NULL);
    return NATIVE_RETURN_VOID();
}

/* Walk the "in" wrapper chain to a ByteArrayInputStream, exactly like
 * media.c's createPlayer (v34.3). */
static bool sax_read_stream(JavaObject* stream, uint8_t** out, int* out_len) {
    *out = NULL;
    *out_len = 0;
    JavaObject* s = stream;
    for (int hop = 0; hop < 5 && s; hop++) {
        JavaClass* sc = s->header.clazz;
        const char* sname = (sc && sc->class_name) ? sc->class_name : NULL;
        if (!sname) break;
        if (strcmp(sname, "java/io/ByteArrayInputStream") == 0) {
            JavaArray* buf = (JavaArray*)native_get_field_value(s, "buf").ref;
            int pos = native_get_field_value(s, "pos").i;
            int count = native_get_field_value(s, "count").i;
            if (buf && count <= 0 && buf->length > 0) count = (int)buf->length;
            if (!buf || count <= 0 || pos < 0 || pos >= count) return false;
            int n = count - pos;
            uint8_t* data = (uint8_t*)malloc((size_t)n);
            if (!data) return false;
            memcpy(data, (uint8_t*)array_data(buf) + pos, (size_t)n);
            *out = data;
            *out_len = n;
            return true;
        }
        s = (JavaObject*)native_get_field_value(s, "in").ref;
    }
    return false;
}

static JavaValue native_saxparser_parse(JVM* jvm, JavaThread* thread,
                                        JavaValue* args, int arg_count) {
    (void)arg_count;
    /* Two overloads share this handler:
     *   parse(InputStream, DefaultHandler)            — args[1] = stream
     *   parse(InputSource, DefaultHandler)            — args[1] = InputSource
     *   parse(InputSource, DefaultHandler, systemId)  — args[1..2] shifted
     * Distinguish by the runtime class of args[1]. */
    JavaObject* first = (JavaObject*)args[1].ref;
    JavaObject* handler = NULL;
    JavaObject* stream = NULL;

    JavaClass* fc = first ? first->header.clazz : NULL;
    const char* fname = (fc && fc->class_name) ? fc->class_name : NULL;
    bool first_is_inputsource =
        (fname && strcmp(fname, "org/xml/sax/InputSource") == 0);

    if (first_is_inputsource) {
        handler = (JavaObject*)args[2].ref;
        stream = sax_inputsource_stream(first);
        if (!stream) {
            /* Fall back to the byteStream/characterStream fields when a
             * game built the InputSource through a path we did not see. */
            stream = (JavaObject*)native_get_field_value(first, "byteStream").ref;
        }
    } else {
        stream = first;
        handler = (JavaObject*)args[2].ref;
    }

    if (!stream) {
        jvm_throw_by_name(jvm, "org/xml/sax/SAXException", "null InputStream");
        return NATIVE_RETURN_VOID();
    }
    if (!handler) {
        jvm_throw_by_name(jvm, "org/xml/sax/SAXException", "null handler");
        return NATIVE_RETURN_VOID();
    }

    uint8_t* data = NULL;
    int len = 0;
    if (!sax_read_stream(stream, &data, &len)) {
        jvm_throw_by_name(jvm, "org/xml/sax/SAXException",
                          "unreadable or empty stream");
        return NATIVE_RETURN_VOID();
    }

    /* One AttributesImpl instance per PARSE (v36.33.1: not per-process! A
     * session-teardown resets the heap without sweeping, so a cached object
     * dangles; in session 2 the game's virtual getValue() calls on the
     * stale object resolved to NULL defaults and Integer.parseInt(null)
     * killed the font parse — NumberFormatException "null" at
     * BitmapFont.attributeAsInt). Its DATA lives in g_sax_attrs and is
     * swapped before every startElement callback. */
    {
        JavaClass* ai = jvm_load_class(jvm, "org/xml/sax/helpers/AttributesImpl");
        g_sax_attrs_obj = ai ? jvm_new_object(jvm, ai) : NULL;
    }

    sax_parse_buffer(jvm, thread, handler, (const char*)data, len);
    free(data);
    return NATIVE_RETURN_VOID();
}

/* ======================================================================
 * Factory + parser plumbing natives
 * ====================================================================== */

static JavaValue native_saxfactory_newInstance(JVM* jvm, JavaThread* thread,
                                               JavaValue* args, int arg_count) {
    (void)thread; (void)args; (void)arg_count;
    JavaClass* c = jvm_load_class(jvm, "javax/xml/parsers/SAXParserFactory");
    if (!c) {
        jvm_throw_by_name(jvm, "org/xml/sax/SAXException",
                          "SAXParserFactory unavailable");
        return NATIVE_RETURN_NULL();
    }
    JavaObject* obj = jvm_new_object(jvm, c);
    return NATIVE_RETURN_OBJECT(obj);
}

static JavaValue native_saxfactory_newSAXParser(JVM* jvm, JavaThread* thread,
                                                JavaValue* args, int arg_count) {
    (void)thread; (void)arg_count;
    JavaObject* self = (JavaObject*)args[0].ref;
    (void)self;
    JavaClass* c = jvm_load_class(jvm, "javax/xml/parsers/SAXParser");
    if (!c) {
        jvm_throw_by_name(jvm, "org/xml/sax/SAXException",
                          "SAXParser unavailable");
        return NATIVE_RETURN_NULL();
    }
    JavaObject* obj = jvm_new_object(jvm, c);
    return NATIVE_RETURN_OBJECT(obj);
}

#define SAX_NOOP_VOID(ret_type)                                        \
    static JavaValue native_sax_noop_##ret_type(JVM* jvm,              \
                                                JavaThread* thread,    \
                                                JavaValue* args,       \
                                                int arg_count)
SAX_NOOP_VOID(void) {
    (void)jvm; (void)thread; (void)args; (void)arg_count;
    return NATIVE_RETURN_VOID();
}

/* ---- AttributesImpl natives: read the CURRENT event's attribute set ---- */

static JavaValue native_saxattrs_getLength(JVM* jvm, JavaThread* thread,
                                           JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)args; (void)arg_count;
    return NATIVE_RETURN_INT(g_sax_attrs.count);
}

static JavaValue native_saxattrs_getQName(JVM* jvm, JavaThread* thread,
                                          JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    int i = args[1].i;
    if (i < 0 || i >= g_sax_attrs.count) return NATIVE_RETURN_NULL();
    return NATIVE_RETURN_OBJECT(jvm_new_string(jvm, g_sax_attrs.qname[i]));
}

static JavaValue native_saxattrs_getValue_idx(JVM* jvm, JavaThread* thread,
                                              JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    int i = args[1].i;
    if (i < 0 || i >= g_sax_attrs.count) return NATIVE_RETURN_NULL();
    return NATIVE_RETURN_OBJECT(jvm_new_string(jvm, g_sax_attrs.value[i]));
}

static JavaValue native_saxattrs_getValue_name(JVM* jvm, JavaThread* thread,
                                               JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    JavaString* name = (JavaString*)args[1].ref;
    if (!name) return NATIVE_RETURN_NULL();
    const char* n = (const char*)string_utf8(jvm, name);
    if (!n) return NATIVE_RETURN_NULL();
    int i = sax_attrs_index(n);
    if (i < 0) return NATIVE_RETURN_NULL();
    return NATIVE_RETURN_OBJECT(jvm_new_string(jvm, g_sax_attrs.value[i]));
}

static JavaValue native_saxattrs_getIndex(JVM* jvm, JavaThread* thread,
                                          JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    JavaString* name = (JavaString*)args[1].ref;
    if (!name) return NATIVE_RETURN_INT(-1);
    const char* n = (const char*)string_utf8(jvm, name);
    if (!n) return NATIVE_RETURN_INT(-1);
    return NATIVE_RETURN_INT(sax_attrs_index(n));
}

static JavaValue native_saxattrs_getType(JVM* jvm, JavaThread* thread,
                                         JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)args; (void)arg_count;
    /* DTD-less documents: every attribute is CDATA */
    return NATIVE_RETURN_OBJECT(jvm_new_string(jvm, "CDATA"));
}

static JavaValue native_saxattrs_getURI_or_local(JVM* jvm, JavaThread* thread,
                                                 JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)args; (void)arg_count;
    /* Namespace-less parser: uri = "" and localName = qName's raw value */
    return NATIVE_RETURN_OBJECT(jvm_new_string(jvm, ""));
}

/* ======================================================================
 * Registration
 * ====================================================================== */

/* v36.33 [SAX-SESSION-RESET]: drop per-process Java-object references held
 * by the SAX layer (the AttributesImpl handed to handlers and the
 * InputSource->stream ring). They point into the dead session's heap after
 * a teardown and would alias garbage in the next session (same class of
 * staleness as the Hashtable peers). */
void sax_session_reset(void) {
    g_sax_attrs_obj = NULL;
    memset(g_sax_inputsrc, 0, sizeof(g_sax_inputsrc));
    g_sax_inputsrc_n = 0;
    sax_attrs_clear();
}

void init_javax_xml_sax(JVM* jvm) {
    static const NativeMethodEntry methods[] = {
        /* Factory */
        {"javax/xml/parsers/SAXParserFactory", "newInstance",
         "()Ljavax/xml/parsers/SAXParserFactory;", native_saxfactory_newInstance},
        {"javax/xml/parsers/SAXParserFactory", "newSAXParser",
         "()Ljavax/xml/parsers/SAXParser;", native_saxfactory_newSAXParser},
        {"javax/xml/parsers/SAXParserFactory", "setValidating", "(Z)V",
         native_sax_noop_void},
        {"javax/xml/parsers/SAXParserFactory", "setNamespaceAware", "(Z)V",
         native_sax_noop_void},
        {"javax/xml/parsers/SAXParserFactory", "setCoalescing", "(Z)V",
         native_sax_noop_void},
        {"javax/xml/parsers/SAXParserFactory", "setExpandEntityReferences", "(Z)V",
         native_sax_noop_void},
        {"javax/xml/parsers/SAXParserFactory", "isNamespaceAware", "()Z",
         native_sax_noop_void},
        {"javax/xml/parsers/SAXParserFactory", "isValidating", "()Z",
         native_sax_noop_void},

        /* Parser */
        {"javax/xml/parsers/SAXParser", "parse",
         "(Ljava/io/InputStream;Lorg/xml/sax/helpers/DefaultHandler;)V",
         native_saxparser_parse},
        {"javax/xml/parsers/SAXParser", "parse",
         "(Lorg/xml/sax/InputSource;Lorg/xml/sax/helpers/DefaultHandler;)V",
         native_saxparser_parse},
        {"javax/xml/parsers/SAXParser", "parse",
         "(Ljava/io/InputStream;Lorg/xml/sax/helpers/DefaultHandler;Ljava/lang/String;)V",
         native_saxparser_parse},
        {"javax/xml/parsers/SAXParser", "parse",
         "(Lorg/xml/sax/InputSource;Lorg/xml/sax/helpers/DefaultHandler;Ljava/lang/String;)V",
         native_saxparser_parse},
        {"javax/xml/parsers/SAXParser", "reset", "()V", native_sax_noop_void},
        {"javax/xml/parsers/SAXParser", "setProperty",
         "(Ljava/lang/String;Ljava/lang/Object;)V", native_sax_noop_void},
        {"javax/xml/parsers/SAXParser", "getProperty",
         "(Ljava/lang/String;)Ljava/lang/Object;", native_sax_noop_void},

        /* InputSource: remember the wrapped stream for parse() */
        {"org/xml/sax/InputSource", "<init>", "(Ljava/io/InputStream;)V",
         native_inputsource_init_stream},
        {"org/xml/sax/InputSource", "<init>", "()V",
         native_inputsource_init_nostream},
        {"org/xml/sax/InputSource", "<init>", "(Ljava/lang/String;)V",
         native_inputsource_init_nostream},
        {"org/xml/sax/InputSource", "setByteStream",
         "(Ljava/io/InputStream;)V", native_inputsource_init_nostream},
        {"org/xml/sax/InputSource", "setSystemId",
         "(Ljava/lang/String;)V", native_sax_noop_void},
        {"org/xml/sax/InputSource", "setEncoding",
         "(Ljava/lang/String;)V", native_sax_noop_void},

        /* DefaultHandler no-op callbacks (games override what they need) */
        {"org/xml/sax/helpers/DefaultHandler", "startDocument", "()V",
         native_sax_noop_void},
        {"org/xml/sax/helpers/DefaultHandler", "endDocument", "()V",
         native_sax_noop_void},
        {"org/xml/sax/helpers/DefaultHandler", "startElement",
         "(Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;"
         "Lorg/xml/sax/Attributes;)V", native_sax_noop_void},
        {"org/xml/sax/helpers/DefaultHandler", "endElement",
         "(Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;)V",
         native_sax_noop_void},
        {"org/xml/sax/helpers/DefaultHandler", "characters", "([CII)V",
         native_sax_noop_void},
        {"org/xml/sax/helpers/DefaultHandler", "ignorableWhitespace", "([CII)V",
         native_sax_noop_void},

        /* AttributesImpl (data served from the current event) */
        {"org/xml/sax/helpers/AttributesImpl", "getLength", "()I",
         native_saxattrs_getLength},
        {"org/xml/sax/helpers/AttributesImpl", "getQName",
         "(I)Ljava/lang/String;", native_saxattrs_getQName},
        {"org/xml/sax/helpers/AttributesImpl", "getValue",
         "(I)Ljava/lang/String;", native_saxattrs_getValue_idx},
        {"org/xml/sax/helpers/AttributesImpl", "getValue",
         "(Ljava/lang/String;)Ljava/lang/String;", native_saxattrs_getValue_name},
        {"org/xml/sax/helpers/AttributesImpl", "getIndex",
         "(Ljava/lang/String;)I", native_saxattrs_getIndex},
        {"org/xml/sax/helpers/AttributesImpl", "getType",
         "(I)Ljava/lang/String;", native_saxattrs_getType},
        {"org/xml/sax/helpers/AttributesImpl", "getType",
         "(Ljava/lang/String;)Ljava/lang/String;", native_saxattrs_getType},
        {"org/xml/sax/helpers/AttributesImpl", "getURI",
         "(I)Ljava/lang/String;", native_saxattrs_getURI_or_local},
        {"org/xml/sax/helpers/AttributesImpl", "getLocalName",
         "(I)Ljava/lang/String;", native_saxattrs_getURI_or_local},
    };
    native_register_methods(jvm, methods, (int)(sizeof(methods) / sizeof(methods[0])));
    SAX_DEBUG("SAX (javax.xml.parsers) natives registered");
}
