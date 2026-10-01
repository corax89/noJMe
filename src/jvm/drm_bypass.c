/*
 * J2ME Emulator - DRM Bypass Implementation
 * Based on KEmulator analysis by user
 * 
 * Implements protection bypass mechanisms similar to KEmulator:
 * - hideEmulation mode
 * - IMEI/IMSI spoofing
 * - Permission bypass
 * - Platform spoofing
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include "drm_bypass.h"
#include "debug.h"
#include "debug_macros.h"

/* Global configuration */
DrmBypassConfig g_drm_bypass;

/* Properties to hide in hideEmulation mode (from KEmulator).
 * v36.25 [SYSPROPS]: префикс "java." УДАЛЁН из списка скрытия — раньше
 * он глотал java.name/java.version/java.vm.name/java.heap.* и подобные
 * (запрос пользователя: «стоит их установить для совместимости»). Наши
 * значения теперь неотличимы от реального телефона (java.version=1.4.2,
 * java.vm.name=CLDC-HI — как у Nokia S60 с Sun CLDC-HI), скрывать больше
 * нечего: спуф сам по себе выглядит легитимно для DRM-детекторов. */
static const char* hidden_property_prefixes[] = {
    "os.",           /* os.name, os.version, os.arch */
    "sun.",          /* Sun-specific properties */
    "org.pigler.",   /* Pigler automation */
    "ru.nnproject.", /* NNProject emulator detection */
    "kemnn.",        /* KEmulator NN detection */
    "emulator.",     /* Generic emulator detection */
    "club.",         /* Club detection */
    "com.github.",   /* GitHub packages */
    NULL
};

/* Properties that reveal emulator presence */
static const char* emulator_revealing_properties[] = {
    "emulator.version",
    "emulator.name",
    "kemulator.version",
    "kemulator.name",
    "nojvme.version",
    "nojvme.name",
    NULL
};

/* Classes to hide in hideEmulation mode (reserved for future use) */
__attribute__((unused))
static const char* hidden_class_prefixes[] = {
    "kemnn.",
    "emulator.",
    "club.",
    "com.github.",
    "nojvme.",
    NULL
};

/* Known DRM/ad detection classes */
static const char* drm_detection_classes[] = {
    "kemnn/",
    "emulator/",
    "club/",
    "com/github/",
    "nojvme/",
    NULL
};

/*
 * Universal DRM bypass class prefix registry.
 *
 * Any class whose fully-qualified name (using '/' separators)
 * starts with one of these prefixes will be treated as a DRM class.
 * This means:
 *   - Null-object method calls return safe defaults (no NPE)
 *   - Null-object field access returns 0/null (no NPE)
 *   - Null array operations return 0 (no NPE)
 *   - Unimplemented native methods return type-appropriate defaults
 *   - Exceptions thrown during startApp are non-fatal
 *
 * To bypass a new DRM system, add its prefix here.
 * Examples of common J2ME DRM systems:
 *   GlomoReg - popular Russian J2ME DRM
 *   GlomoRegStarter - entry point for GlomoReg
 *   GlomoReg/GlomoCountry - country-based licensing
 *   GlomoReg/GlomoConfig - configuration
 *   GlomoReg/GlomoRMS - record store
 */
static const char* g_drm_bypass_prefixes[DRM_MAX_PREFIXES] = {
    /* GlomoReg - popular J2ME copy protection system */
    "GlomoReg/",
    /* GlomoRegStarter - entry class (not in GlomoReg/ package) */
    "GlomoRegStarter",
    /* Add more DRM prefixes here as needed, e.g.: */
    /* "SomeOtherDRM/", */
    /* "com/protection/", */
    /* "LicenseManager", */
    NULL  /* must be last */
};
static int g_drm_bypass_count = 2;  /* number of non-NULL entries */

void drm_bypass_init(void) {
    memset(&g_drm_bypass, 0, sizeof(g_drm_bypass));
    
    /* Default settings - enable all bypasses for maximum compatibility */
    g_drm_bypass.hide_emulation = true;
    g_drm_bypass.bypass_permissions = true;
    g_drm_bypass.bypass_vserv = true;
    g_drm_bypass.debug_drm = false;
    
    /* Default spoofed values (Nokia N73-like) */
    strcpy(g_drm_bypass.imei, "000000000000000");
    strcpy(g_drm_bypass.imsi, "000000000000000");
    strcpy(g_drm_bypass.platform, "NokiaN73-1");
    strcpy(g_drm_bypass.device_model, "Nokia N73");
    
    NATIVE_DEBUG("[DRM] Bypass initialized: hide=%d, permissions=%d, vserv=%d",
            g_drm_bypass.hide_emulation, g_drm_bypass.bypass_permissions,
            g_drm_bypass.bypass_vserv);
}

void drm_bypass_load_config(const char* filename) {
    FILE* f = fopen(filename, "r");
    if (!f) {
        NATIVE_DEBUG("[DRM] Config file not found: %s (using defaults)", filename);
        return;
    }
    
    char line[256];
    while (fgets(line, sizeof(line), f)) {
        /* Skip comments and empty lines */
        if (line[0] == '#' || line[0] == '\n' || line[0] == '\r') continue;
        
        /* Parse: key = value */
        char* eq = strchr(line, '=');
        if (!eq) continue;
        
        *eq = '\0';
        char* key = line;
        char* value = eq + 1;
        
        /* Trim whitespace */
        while (*key == ' ' || *key == '\t') key++;
        while (*value == ' ' || *value == '\t') value++;
        
        /* Remove trailing whitespace from value */
        char* end = value + strlen(value) - 1;
        while (end > value && (*end == '\n' || *end == '\r' || *end == ' ')) {
            *end-- = '\0';
        }
        
        /* Parse settings */
        if (strcmp(key, "hide_emulation") == 0) {
            g_drm_bypass.hide_emulation = (strcmp(value, "true") == 0 || strcmp(value, "1") == 0);
        } else if (strcmp(key, "bypass_permissions") == 0) {
            g_drm_bypass.bypass_permissions = (strcmp(value, "true") == 0 || strcmp(value, "1") == 0);
        } else if (strcmp(key, "bypass_vserv") == 0) {
            g_drm_bypass.bypass_vserv = (strcmp(value, "true") == 0 || strcmp(value, "1") == 0);
        } else if (strcmp(key, "imei") == 0) {
            strncpy(g_drm_bypass.imei, value, sizeof(g_drm_bypass.imei) - 1);
        } else if (strcmp(key, "imsi") == 0) {
            strncpy(g_drm_bypass.imsi, value, sizeof(g_drm_bypass.imsi) - 1);
        } else if (strcmp(key, "platform") == 0) {
            strncpy(g_drm_bypass.platform, value, sizeof(g_drm_bypass.platform) - 1);
        } else if (strcmp(key, "device_model") == 0) {
            strncpy(g_drm_bypass.device_model, value, sizeof(g_drm_bypass.device_model) - 1);
        } else if (strcmp(key, "debug_drm") == 0) {
            g_drm_bypass.debug_drm = (strcmp(value, "true") == 0 || strcmp(value, "1") == 0);
        }
    }
    
    fclose(f);
    NATIVE_DEBUG("[DRM] Config loaded: hide=%d, imei=%s, platform=%s",
            g_drm_bypass.hide_emulation, g_drm_bypass.imei, g_drm_bypass.platform);
}

bool drm_should_hide_property(const char* key) {
    if (!g_drm_bypass.hide_emulation) return false;
    if (!key) return false;
    
    /* Check hidden prefixes */
    for (int i = 0; hidden_property_prefixes[i]; i++) {
        if (strncmp(key, hidden_property_prefixes[i], 
                    strlen(hidden_property_prefixes[i])) == 0) {
            if (g_drm_bypass.debug_drm) {
                LOG_SAFE("[DRM] Hiding property: %s\n", key);
            }
            return true;
        }
    }
    
    /* Check emulator-revealing properties */
    for (int i = 0; emulator_revealing_properties[i]; i++) {
        if (strcmp(key, emulator_revealing_properties[i]) == 0) {
            if (g_drm_bypass.debug_drm) {
                LOG_SAFE("[DRM] Hiding emulator property: %s\n", key);
            }
            return true;
        }
    }
    
    return false;
}

const char* drm_get_spoofed_property(const char* key) {
    if (!key) return NULL;
    
    /* IMEI spoofing - various property names used by different manufacturers */
    if (strcmp(key, "com.nokia.mid.imei") == 0 ||
        strcmp(key, "device.imei") == 0 ||
        strcmp(key, "phone.imei") == 0 ||
        strcmp(key, "com.sonyericsson.imei") == 0 ||
        strcmp(key, "imei") == 0 ||
        strcmp(key, "IMEI") == 0) {
        return g_drm_bypass.imei;
    }
    
    /* IMSI spoofing */
    if (strcmp(key, "com.nokia.mid.imsi") == 0 ||
        strcmp(key, "device.imsi") == 0 ||
        strcmp(key, "phone.imsi") == 0 ||
        strcmp(key, "imsi") == 0 ||
        strcmp(key, "IMSI") == 0) {
        return g_drm_bypass.imsi;
    }
    
    /* Platform spoofing */
    if (strcmp(key, "microedition.platform") == 0) {
        return g_drm_bypass.platform[0] ? g_drm_bypass.platform : "NokiaN73-1";
    }
    
    /* Device model */
    if (strcmp(key, "device.model") == 0 ||
        strcmp(key, "device.model.name") == 0) {
        return g_drm_bypass.device_model[0] ? g_drm_bypass.device_model : "Nokia N73";
    }
    
    /* Nokia-specific */
    if (strcmp(key, "com.nokia.mid.ui.version") == 0) {
        return "1.0";
    }
    if (strcmp(key, "com.nokia.mid.ui.fullscreen") == 0) {
        return "true";
    }
    
    /* Siemens-specific */
    if (strcmp(key, "com.siemens.mp.screenwidth") == 0) {
        return "240";
    }
    if (strcmp(key, "com.siemens.mp.screenheight") == 0) {
        return "320";
    }
    
    /* Motorola-specific */
    if (strcmp(key, "motorola.microedition.locale") == 0) {
        /* v36.25: тот же источник, что и microedition.locale — язык Switch
         * с учётом MIDlet-Languages мидлета ([LOCALE-COMPAT]). */
        extern const char* nojme_sysprop_locale(void);
        const char* c = nojme_sysprop_locale();
        return (c && c[0]) ? c : "en";
    }
    
    /* Samsung-specific */
    if (strcmp(key, "samsung.mime") == 0) {
        return "true";
    }
    
    /* Sony Ericsson-specific */
    if (strcmp(key, "com.sonyericsson.imei") == 0) {
        return g_drm_bypass.imei;
    }
    
    return NULL;
}

bool drm_should_hide_class(const char* class_name) {
    if (!g_drm_bypass.hide_emulation) return false;
    if (!class_name) return false;
    
    for (int i = 0; drm_detection_classes[i]; i++) {
        if (strncmp(class_name, drm_detection_classes[i], 
                    strlen(drm_detection_classes[i])) == 0) {
            return true;
        }
    }
    
    return false;
}

const char* drm_get_imei(void) {
    return g_drm_bypass.imei;
}

int drm_check_permission(const char* permission) {
    (void)permission; /* Not used when bypassing all */
    
    if (g_drm_bypass.bypass_permissions) {
        /* KEmulator returns 1 (allowed) for all permissions */
        return 1;
    }
    
    /* Unknown - let system handle it */
    return -1;
}

/* ============ Universal DRM class bypass system ============ */

bool drm_is_drm_class(const char* class_name) {
    if (!class_name) return false;
    
    for (int i = 0; i < g_drm_bypass_count; i++) {
        if (g_drm_bypass_prefixes[i] &&
            strncmp(class_name, g_drm_bypass_prefixes[i],
                    strlen(g_drm_bypass_prefixes[i])) == 0) {
            if (g_drm_bypass.debug_drm) {
                LOG_SAFE("[DRM] Bypassing class: %s (matched prefix: %s)\n",
                        class_name, g_drm_bypass_prefixes[i]);
            }
            return true;
        }
    }
    
    return false;
}

char drm_get_default_return_type(const char* descriptor) {
    if (!descriptor || !descriptor[0]) return '\0';
    
    /* Return type is the last character of the method descriptor */
    size_t len = strlen(descriptor);
    if (len == 0) return '\0';
    
    char last = descriptor[len - 1];
    
    /* Sanity check: valid return type characters */
    switch (last) {
        case 'V':  /* void */
        case 'Z':  /* boolean */
        case 'B':  /* byte */
        case 'C':  /* char */
        case 'S':  /* short */
        case 'I':  /* int */
        case 'J':  /* long */
        case 'F':  /* float */
        case 'D':  /* double */
        case ';':  /* object reference (ends with L...;) */
        case ']':  /* array reference (ends with [...;) or [...[) */
            return last;
        default:
            return '\0';
    }
}

void drm_add_bypass_prefix(const char* prefix) {
    if (!prefix || !prefix[0]) return;
    
    if (g_drm_bypass_count >= DRM_MAX_PREFIXES - 1) {
        LOG_SAFE("[DRM] Cannot add prefix '%s': registry full (max %d)\n",
                prefix, DRM_MAX_PREFIXES - 1);
        return;
    }
    
    /* Check for duplicates */
    size_t plen = strlen(prefix);
    for (int i = 0; i < g_drm_bypass_count; i++) {
        if (g_drm_bypass_prefixes[i] &&
            strncmp(g_drm_bypass_prefixes[i], prefix, plen + 1) == 0) {
            return;  /* Already registered */
        }
    }
    
    g_drm_bypass_prefixes[g_drm_bypass_count] = prefix;
    g_drm_bypass_count++;
    g_drm_bypass_prefixes[g_drm_bypass_count] = NULL;  /* maintain NULL terminator */
    
    LOG_SAFE("[DRM] Registered bypass prefix: %s (total: %d)\n", prefix, g_drm_bypass_count);
}

const char* const* drm_get_bypass_prefixes(void) {
    return g_drm_bypass_prefixes;
}
