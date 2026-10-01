/*
 * J2ME Emulator - Built-in Stub Classes
 * Generates minimal stub classes for J2ME API
 */

#ifndef _WIN32
#define _POSIX_C_SOURCE 200809L  /* For strdup - only on POSIX systems */
#endif

#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include <string.h>

#include "jvm.h"
#include "classfile.h"
#include "native.h"
#include "opcodes.h"  /* For ACC_* constants */
#include "debug.h"
#include "debug_macros.h"

/* Forward declarations */
static JavaClass* create_stub_class(JVM* jvm, const char* name, const char* super_name, uint16_t access_flags);

/* v34.15 FIX (Galaxy on Fire: ClassCastException on VolumeControl):
   wire the "XyzImpl implements Xyz" links for concrete media stubs.
   This table used to live ONLY inside get_or_create_stub_class(), but all
   five classes already exist by then — init_stub_classes() creates them at
   boot straight from the static stub_classes[] table WITHOUT interfaces.
   get_or_create_stub_class() then found them "already loaded" and returned
   them interface-less, so checkcast VolumeControl failed with CCE the first
   time a game called Player.getControl("VolumeControl"). Extracted here and
   called from BOTH creation paths. */
static void stub_add_known_interfaces(JavaClass* stub);


/* List of required J2ME stub classes */
static const struct {
    const char* name;
    const char* super;
    uint16_t flags;
} stub_classes[] = {
    /* Core Java classes */
    /* FIX: Object must be concrete (not abstract) to have a constructor */
    { "java/lang/Object", NULL, ACC_PUBLIC },
    { "java/lang/Class", "java/lang/Object", ACC_PUBLIC },
    { "java/lang/String", "java/lang/Object", ACC_PUBLIC | ACC_FINAL },
    { "java/lang/StringBuilder", "java/lang/Object", ACC_PUBLIC },
    { "java/lang/StringBuffer", "java/lang/Object", ACC_PUBLIC },
    { "java/lang/Throwable", "java/lang/Object", ACC_PUBLIC },
    { "java/lang/StackTraceElement", "java/lang/Object", ACC_PUBLIC },
    { "java/lang/Exception", "java/lang/Throwable", ACC_PUBLIC },
    { "java/lang/RuntimeException", "java/lang/Exception", ACC_PUBLIC },
    { "java/lang/Error", "java/lang/Throwable", ACC_PUBLIC },
    { "java/lang/NullPointerException", "java/lang/RuntimeException", ACC_PUBLIC },
    { "java/lang/ClassCastException", "java/lang/RuntimeException", ACC_PUBLIC },
    { "java/lang/ArrayIndexOutOfBoundsException", "java/lang/RuntimeException", ACC_PUBLIC },
    { "java/lang/IndexOutOfBoundsException", "java/lang/RuntimeException", ACC_PUBLIC },
    { "java/lang/IllegalArgumentException", "java/lang/RuntimeException", ACC_PUBLIC },
    { "java/lang/IllegalStateException", "java/lang/RuntimeException", ACC_PUBLIC },
    { "java/lang/NumberFormatException", "java/lang/IllegalArgumentException", ACC_PUBLIC },
    { "java/lang/ClassNotFoundException", "java/lang/Exception", ACC_PUBLIC },
    { "java/lang/OutOfMemoryError", "java/lang/Error", ACC_PUBLIC },
    { "java/lang/VirtualMachineError", "java/lang/Error", ACC_PUBLIC },
    { "java/lang/InternalError", "java/lang/VirtualMachineError", ACC_PUBLIC },
    { "java/lang/NoSuchMethodError", "java/lang/Error", ACC_PUBLIC },
    { "java/lang/NoSuchFieldError", "java/lang/Error", ACC_PUBLIC },
    { "java/lang/NoClassDefFoundError", "java/lang/Error", ACC_PUBLIC },
    { "java/lang/ClassFormatError", "java/lang/Error", ACC_PUBLIC },
    { "java/lang/VerifyError", "java/lang/Error", ACC_PUBLIC },
    { "java/lang/Integer", "java/lang/Object", ACC_PUBLIC },
    { "java/lang/Long", "java/lang/Object", ACC_PUBLIC },
    { "java/lang/Float", "java/lang/Object", ACC_PUBLIC },
    { "java/lang/Double", "java/lang/Object", ACC_PUBLIC },
    { "java/lang/Boolean", "java/lang/Object", ACC_PUBLIC },
    { "java/lang/Math", "java/lang/Object", ACC_PUBLIC },
    { "java/lang/System", "java/lang/Object", ACC_PUBLIC },
    { "java/lang/Runtime", "java/lang/Object", ACC_PUBLIC },
    { "java/lang/Thread", "java/lang/Object", ACC_PUBLIC },
    { "java/lang/Runnable", "java/lang/Object", ACC_PUBLIC | ACC_INTERFACE },
    { "java/lang/Cloneable", "java/lang/Object", ACC_PUBLIC | ACC_INTERFACE },
    { "java/util/Random", "java/lang/Object", ACC_PUBLIC },
    { "java/util/Vector", "java/lang/Object", ACC_PUBLIC },
    { "java/util/Hashtable", "java/lang/Object", ACC_PUBLIC },
    { "java/util/Enumeration", "java/lang/Object", ACC_PUBLIC | ACC_INTERFACE },
    { "java/util/Timer", "java/lang/Object", ACC_PUBLIC },
    { "java/util/TimerTask", "java/lang/Object", ACC_PUBLIC | ACC_ABSTRACT },

    /* v36.33 [SAX-XML]: javax.xml.parsers + org.xml.sax stubs (Speedx 3D
     * and other games parse their bitmap-font descriptors through SAX).
     * The natives live in jvm/sax.c. */
    { "javax/xml/parsers/SAXParserFactory", "java/lang/Object", ACC_PUBLIC },
    { "javax/xml/parsers/SAXParser", "java/lang/Object", ACC_PUBLIC },
    { "javax/xml/parsers/ParserConfigurationException", "java/lang/Exception", ACC_PUBLIC },
    { "org/xml/sax/SAXException", "java/lang/Exception", ACC_PUBLIC },
    { "org/xml/sax/SAXNotRecognizedException", "org/xml/sax/SAXException", ACC_PUBLIC },
    { "org/xml/sax/SAXNotSupportedException", "org/xml/sax/SAXException", ACC_PUBLIC },
    { "org/xml/sax/InputSource", "java/lang/Object", ACC_PUBLIC },
    { "org/xml/sax/Attributes", "java/lang/Object", ACC_PUBLIC | ACC_INTERFACE },
    { "org/xml/sax/ContentHandler", "java/lang/Object", ACC_PUBLIC | ACC_INTERFACE },
    { "org/xml/sax/helpers/DefaultHandler", "java/lang/Object", ACC_PUBLIC },
    { "org/xml/sax/helpers/AttributesImpl", "java/lang/Object", ACC_PUBLIC },
    
    /* I/O classes */
    { "java/io/InputStream", "java/lang/Object", ACC_PUBLIC | ACC_ABSTRACT },
    { "java/io/OutputStream", "java/lang/Object", ACC_PUBLIC | ACC_ABSTRACT },
    { "java/io/Reader", "java/lang/Object", ACC_PUBLIC | ACC_ABSTRACT },
    { "java/io/InputStreamReader", "java/io/Reader", ACC_PUBLIC },
    { "java/io/BufferedReader", "java/io/Reader", ACC_PUBLIC },
    { "java/io/ByteArrayInputStream", "java/io/InputStream", ACC_PUBLIC },
    { "java/io/ByteArrayOutputStream", "java/io/OutputStream", ACC_PUBLIC },
    { "java/io/DataInputStream", "java/io/InputStream", ACC_PUBLIC },
    { "java/io/DataOutputStream", "java/io/OutputStream", ACC_PUBLIC },
    { "java/io/IOException", "java/lang/Exception", ACC_PUBLIC },
    { "java/io/EOFException", "java/io/IOException", ACC_PUBLIC },
    { "java/io/UTFDataFormatException", "java/io/IOException", ACC_PUBLIC },
    { "java/io/FileNotFoundException", "java/io/IOException", ACC_PUBLIC },
    { "java/io/InterruptedIOException", "java/io/IOException", ACC_PUBLIC },
    { "java/io/PrintStream", "java/io/OutputStream", ACC_PUBLIC },
    { "java/io/UnsupportedEncodingException", "java/io/IOException", ACC_PUBLIC },
    
    /* J2ME MIDlet API */
    { "javax/microedition/midlet/MIDlet", "java/lang/Object", ACC_PUBLIC | ACC_ABSTRACT },
    { "javax/microedition/midlet/MIDletStateChangeException", "java/lang/Exception", ACC_PUBLIC },
    
    /* J2ME LCDUI API */
    { "javax/microedition/lcdui/Display", "java/lang/Object", ACC_PUBLIC },
    { "javax/microedition/lcdui/Displayable", "java/lang/Object", ACC_PUBLIC | ACC_ABSTRACT },
    { "javax/microedition/lcdui/Screen", "javax/microedition/lcdui/Displayable", ACC_PUBLIC | ACC_ABSTRACT },
    { "javax/microedition/lcdui/Canvas", "javax/microedition/lcdui/Displayable", ACC_PUBLIC | ACC_ABSTRACT },
    { "javax/microedition/lcdui/game/GameCanvas", "javax/microedition/lcdui/Canvas", ACC_PUBLIC },
    { "javax/microedition/lcdui/game/Layer", "java/lang/Object", ACC_PUBLIC | ACC_ABSTRACT },
    { "javax/microedition/lcdui/game/Sprite", "javax/microedition/lcdui/game/Layer", ACC_PUBLIC },
    { "javax/microedition/lcdui/game/TiledLayer", "javax/microedition/lcdui/game/Layer", ACC_PUBLIC },
    { "javax/microedition/lcdui/game/LayerManager", "java/lang/Object", ACC_PUBLIC },
    { "javax/microedition/lcdui/Graphics", "java/lang/Object", ACC_PUBLIC }, 
    { "javax/microedition/lcdui/Image", "java/lang/Object", ACC_PUBLIC | ACC_FINAL },
    { "javax/microedition/lcdui/Font", "java/lang/Object", ACC_PUBLIC },
    { "javax/microedition/lcdui/Command", "java/lang/Object", ACC_PUBLIC },
    { "javax/microedition/lcdui/CommandListener", "java/lang/Object", ACC_PUBLIC | ACC_INTERFACE },
    { "javax/microedition/lcdui/Item", "java/lang/Object", ACC_PUBLIC | ACC_ABSTRACT },
    { "javax/microedition/lcdui/Form", "javax/microedition/lcdui/Screen", ACC_PUBLIC },
    { "javax/microedition/lcdui/TextBox", "javax/microedition/lcdui/Screen", ACC_PUBLIC },
    { "javax/microedition/lcdui/List", "javax/microedition/lcdui/Screen", ACC_PUBLIC },
    { "javax/microedition/lcdui/Alert", "javax/microedition/lcdui/Screen", ACC_PUBLIC },
    { "javax/microedition/lcdui/AlertType", "java/lang/Object", ACC_PUBLIC },
    { "javax/microedition/lcdui/Ticker", "java/lang/Object", ACC_PUBLIC },
    { "javax/microedition/lcdui/Gauge", "javax/microedition/lcdui/Item", ACC_PUBLIC },
    { "javax/microedition/lcdui/StringItem", "javax/microedition/lcdui/Item", ACC_PUBLIC },
    { "javax/microedition/lcdui/TextField", "javax/microedition/lcdui/Item", ACC_PUBLIC },
    { "javax/microedition/lcdui/ChoiceGroup", "javax/microedition/lcdui/Item", ACC_PUBLIC },
    { "javax/microedition/lcdui/ImageItem", "javax/microedition/lcdui/Item", ACC_PUBLIC },
    { "javax/microedition/lcdui/Spacer", "javax/microedition/lcdui/Item", ACC_PUBLIC },
    { "javax/microedition/lcdui/DateField", "javax/microedition/lcdui/Item", ACC_PUBLIC },
    
    /* J2ME RMS API */
    { "javax/microedition/rms/RecordStore", "java/lang/Object", ACC_PUBLIC },
    { "javax/microedition/rms/RecordEnumeration", "java/lang/Object", ACC_PUBLIC | ACC_INTERFACE },
    { "javax/microedition/rms/RecordEnumerationImpl", "java/lang/Object", ACC_PUBLIC },  /* Concrete implementation */
    { "javax/microedition/rms/RecordFilter", "java/lang/Object", ACC_PUBLIC | ACC_INTERFACE },
    { "javax/microedition/rms/RecordComparator", "java/lang/Object", ACC_PUBLIC | ACC_INTERFACE },
    { "javax/microedition/rms/RecordStoreException", "java/lang/Exception", ACC_PUBLIC },
    { "javax/microedition/rms/RecordStoreNotFoundException", "javax/microedition/rms/RecordStoreException", ACC_PUBLIC },
    { "javax/microedition/rms/RecordStoreFullException", "javax/microedition/rms/RecordStoreException", ACC_PUBLIC },
    { "javax/microedition/rms/RecordStoreNotOpenException", "javax/microedition/rms/RecordStoreException", ACC_PUBLIC },
    { "javax/microedition/rms/InvalidRecordIDException", "javax/microedition/rms/RecordStoreException", ACC_PUBLIC },
    { "javax/microedition/rms/RecordListener", "java/lang/Object", ACC_PUBLIC | ACC_INTERFACE },
    
    /* J2ME Media API - JSR-135 Complete Implementation */
    { "javax/microedition/media/Manager", "java/lang/Object", ACC_PUBLIC },
    { "javax/microedition/media/Player", "java/lang/Object", ACC_PUBLIC | ACC_INTERFACE },
    { "javax/microedition/media/PlayerImpl", "java/lang/Object", ACC_PUBLIC },  /* Concrete Player implementation */
    { "javax/microedition/media/Control", "java/lang/Object", ACC_PUBLIC | ACC_INTERFACE },
    { "javax/microedition/media/Controllable", "java/lang/Object", ACC_PUBLIC | ACC_INTERFACE },
    { "javax/microedition/media/MediaException", "java/lang/Exception", ACC_PUBLIC },
    { "javax/microedition/media/PlayerListener", "java/lang/Object", ACC_PUBLIC | ACC_INTERFACE },
    { "javax/microedition/media/TimeBase", "java/lang/Object", ACC_PUBLIC },
    
    /* VolumeControl */
    { "javax/microedition/media/control/VolumeControl", "java/lang/Object", ACC_PUBLIC | ACC_INTERFACE },
    { "javax/microedition/media/control/VolumeControlImpl", "java/lang/Object", ACC_PUBLIC },  /* Concrete implementation */
    
    /* MIDIControl */
    { "javax/microedition/media/control/MIDIControl", "java/lang/Object", ACC_PUBLIC | ACC_INTERFACE },
    { "javax/microedition/media/control/MIDIControlImpl", "java/lang/Object", ACC_PUBLIC },  /* Concrete implementation */
    
    /* ToneControl */
    { "javax/microedition/media/control/ToneControl", "java/lang/Object", ACC_PUBLIC | ACC_INTERFACE },
    { "javax/microedition/media/control/ToneControlImpl", "java/lang/Object", ACC_PUBLIC },
    
    /* VideoControl */
    { "javax/microedition/media/control/VideoControl", "java/lang/Object", ACC_PUBLIC | ACC_INTERFACE },
    { "javax/microedition/media/control/VideoControlImpl", "java/lang/Object", ACC_PUBLIC },
    
    /* GUIControl */
    { "javax/microedition/media/control/GUIControl", "java/lang/Object", ACC_PUBLIC | ACC_INTERFACE },
    
    /* StopTimeControl */
    { "javax/microedition/media/control/StopTimeControl", "java/lang/Object", ACC_PUBLIC | ACC_INTERFACE },
    
    /* FramePositioningControl */
    { "javax/microedition/media/control/FramePositioningControl", "java/lang/Object", ACC_PUBLIC | ACC_INTERFACE },
    
    /* RateControl */
    { "javax/microedition/media/control/RateControl", "java/lang/Object", ACC_PUBLIC | ACC_INTERFACE },
    
    /* PitchControl */
    { "javax/microedition/media/control/PitchControl", "java/lang/Object", ACC_PUBLIC | ACC_INTERFACE },
    
    /* TempoControl */
    { "javax/microedition/media/control/TempoControl", "java/lang/Object", ACC_PUBLIC | ACC_INTERFACE },
    
    /* Nokia UI/Sound API */
    { "com/nokia/mid/ui/FullCanvas", "javax/microedition/lcdui/Canvas", ACC_PUBLIC },
    { "com/nokia/mid/ui/DirectGraphics", "java/lang/Object", ACC_PUBLIC | ACC_INTERFACE },
    { "com/nokia/mid/ui/DirectUtils", "java/lang/Object", ACC_PUBLIC },
    { "com/nokia/mid/ui/DeviceControl", "java/lang/Object", ACC_PUBLIC },
    /* [BATFIX] v36.41: собственный класс-API заряда для мидлетов —
     * nojme.device.Battery.getLevel()I / isCharging()Z (нативы
     * init_nojme_battery в native.c; диспетчер invokestatic резолвит их
     * по имени класса даже без объявленных методов стаба — тот же путь,
     * что у DeviceControl). Второй способ чтения — System.getProperty
     * (ключи batterylevel / com.nokia.mid.batterylevel /
     * nojme.batterylevel), см. sysprop_dynamic. */
    { "nojme/device/Battery", "java/lang/Object", ACC_PUBLIC },
    { "com/nokia/mid/sound/Sound", "java/lang/Object", ACC_PUBLIC },
    { "com/nokia/mid/sound/SoundListener", "java/lang/Object", ACC_PUBLIC | ACC_INTERFACE },
    { "com/nokia/mid/ui/Clipboard", "java/lang/Object", ACC_PUBLIC },
    { "com/nokia/mid/m3d/M3D", "java/lang/Object", ACC_PUBLIC },
    { "com/nokia/mid/m3d/Texture", "java/lang/Object", ACC_PUBLIC },
    { "com/nokia/mid/m3d/M3DException", "java/lang/Exception", ACC_PUBLIC },
    { "com/nokia/mid/ui/SoftNotification", "java/lang/Object", ACC_PUBLIC },
    { "com/nokia/mid/ui/SoftNotificationListener", "java/lang/Object", ACC_PUBLIC | ACC_INTERFACE },
    { "com/nokia/mid/ui/SoftNotificationException", "java/lang/Exception", ACC_PUBLIC },
    
    /* Additional java.lang wrapper classes */
    { "java/lang/Byte", "java/lang/Object", ACC_PUBLIC },
    { "java/lang/Short", "java/lang/Object", ACC_PUBLIC },
    { "java/lang/Character", "java/lang/Object", ACC_PUBLIC },
    { "java/lang/Void", "java/lang/Object", ACC_PUBLIC },
    { "java/lang/SecurityException", "java/lang/RuntimeException", ACC_PUBLIC },
    { "java/lang/UnsupportedOperationException", "java/lang/RuntimeException", ACC_PUBLIC },
    { "java/lang/InterruptedException", "java/lang/Exception", ACC_PUBLIC },
    
    /* Additional java.util classes */
    { "java/util/Stack", "java/util/Vector", ACC_PUBLIC },
    { "java/util/Calendar", "java/lang/Object", ACC_PUBLIC | ACC_ABSTRACT },
    { "java/util/Date", "java/lang/Object", ACC_PUBLIC },
    { "java/util/TimeZone", "java/lang/Object", ACC_PUBLIC | ACC_ABSTRACT },
    { "java/util/Comparator", "java/lang/Object", ACC_PUBLIC | ACC_INTERFACE },
    { "java/util/EventListener", "java/lang/Object", ACC_PUBLIC | ACC_INTERFACE },
    { "java/util/NoSuchElementException", "java/lang/RuntimeException", ACC_PUBLIC },
    { "java/util/TooManyListenersException", "java/lang/Exception", ACC_PUBLIC },
    
    /* J2ME IO (GCF - Generic Connection Framework) */
    { "javax/microedition/io/Connector", "java/lang/Object", ACC_PUBLIC },
    { "javax/microedition/io/Connection", "java/lang/Object", ACC_PUBLIC | ACC_INTERFACE },
    { "javax/microedition/io/InputConnection", "javax/microedition/io/Connection", ACC_PUBLIC | ACC_INTERFACE },
    { "javax/microedition/io/OutputConnection", "javax/microedition/io/Connection", ACC_PUBLIC | ACC_INTERFACE },
    { "javax/microedition/io/StreamConnection", "javax/microedition/io/Connection", ACC_PUBLIC | ACC_INTERFACE },
    { "javax/microedition/io/StreamConnectionNotifier", "javax/microedition/io/Connection", ACC_PUBLIC | ACC_INTERFACE },
    { "javax/microedition/io/ContentConnection", "javax/microedition/io/StreamConnection", ACC_PUBLIC | ACC_INTERFACE },
    { "javax/microedition/io/HttpConnection", "javax/microedition/io/ContentConnection", ACC_PUBLIC | ACC_INTERFACE },
    { "javax/microedition/io/HttpsConnection", "javax/microedition/io/HttpConnection", ACC_PUBLIC | ACC_INTERFACE },
    { "javax/microedition/io/SocketConnection", "javax/microedition/io/StreamConnection", ACC_PUBLIC | ACC_INTERFACE },
    { "javax/microedition/io/ServerSocketConnection", "javax/microedition/io/Connection", ACC_PUBLIC | ACC_INTERFACE },
    { "javax/microedition/io/DatagramConnection", "javax/microedition/io/Connection", ACC_PUBLIC | ACC_INTERFACE },
    { "javax/microedition/io/Datagram", "java/lang/Object", ACC_PUBLIC | ACC_INTERFACE },
    { "javax/microedition/io/PushRegistry", "java/lang/Object", ACC_PUBLIC },
    { "javax/microedition/io/ConnectionNotFoundException", "java/io/IOException", ACC_PUBLIC },
    { "javax/microedition/io/SecurityInfo", "java/lang/Object", ACC_PUBLIC },
    
    /* J2ME PIM API (Personal Information Management) */
    { "javax/microedition/pim/PIM", "java/lang/Object", ACC_PUBLIC },
    { "javax/microedition/pim/PIMItem", "java/lang/Object", ACC_PUBLIC | ACC_INTERFACE },
    { "javax/microedition/pim/PIMList", "java/lang/Object", ACC_PUBLIC | ACC_INTERFACE },
    { "javax/microedition/pim/Contact", "javax/microedition/pim/PIMItem", ACC_PUBLIC | ACC_INTERFACE },
    { "javax/microedition/pim/ContactList", "javax/microedition/pim/PIMList", ACC_PUBLIC | ACC_INTERFACE },
    { "javax/microedition/pim/Event", "javax/microedition/pim/PIMItem", ACC_PUBLIC | ACC_INTERFACE },
    { "javax/microedition/pim/EventList", "javax/microedition/pim/PIMList", ACC_PUBLIC | ACC_INTERFACE },
    { "javax/microedition/pim/ToDo", "javax/microedition/pim/PIMItem", ACC_PUBLIC | ACC_INTERFACE },
    { "javax/microedition/pim/ToDoList", "javax/microedition/pim/PIMList", ACC_PUBLIC | ACC_INTERFACE },
    { "javax/microedition/pim/PIMException", "java/lang/Exception", ACC_PUBLIC },
    { "javax/microedition/pim/PIMFieldException", "javax/microedition/pim/PIMException", ACC_PUBLIC },
    { "javax/microedition/pim/UnsupportedFieldException", "javax/microedition/pim/PIMException", ACC_PUBLIC },
    
    /* J2ME Media additional controls */
    { "javax/microedition/media/control/VideoControl", "javax/microedition/media/Control", ACC_PUBLIC | ACC_INTERFACE },
    { "javax/microedition/media/control/ToneControl", "javax/microedition/media/Control", ACC_PUBLIC | ACC_INTERFACE },
    { "javax/microedition/media/control/MIDIControl", "javax/microedition/media/Control", ACC_PUBLIC | ACC_INTERFACE },
    { "javax/microedition/media/Control", "java/lang/Object", ACC_PUBLIC | ACC_INTERFACE },
    { "javax/microedition/media/Controllable", "java/lang/Object", ACC_PUBLIC | ACC_INTERFACE },
    { "javax/microedition/media/MediaException", "java/lang/Exception", ACC_PUBLIC },
    { "javax/microedition/media/PlayerListener", "java/lang/Object", ACC_PUBLIC | ACC_INTERFACE },
    
    /* J2ME LCDUI additional classes */
    { "javax/microedition/lcdui/CustomItem", "javax/microedition/lcdui/Item", ACC_PUBLIC | ACC_ABSTRACT },
    { "javax/microedition/lcdui/ItemCommandListener", "java/lang/Object", ACC_PUBLIC | ACC_INTERFACE },
    { "javax/microedition/lcdui/ItemStateListener", "java/lang/Object", ACC_PUBLIC | ACC_INTERFACE },
    { "javax/microedition/lcdui/Choice", "java/lang/Object", ACC_PUBLIC | ACC_INTERFACE },
    { "javax/microedition/lcdui/Graphics$Translate", "java/lang/Object", ACC_PUBLIC },
    { "javax/microedition/lcdui/Image$ImageData", "java/lang/Object", ACC_PUBLIC },
    
    /* J2ME Location API (JSR-179) */
    { "javax/microedition/location/Location", "java/lang/Object", ACC_PUBLIC },
    { "javax/microedition/location/Coordinates", "java/lang/Object", ACC_PUBLIC },
    { "javax/microedition/location/LocationProvider", "java/lang/Object", ACC_PUBLIC },
    { "javax/microedition/location/LocationListener", "java/lang/Object", ACC_PUBLIC | ACC_INTERFACE },
    { "javax/microedition/location/Criteria", "java/lang/Object", ACC_PUBLIC },
    { "javax/microedition/location/LocationException", "java/lang/Exception", ACC_PUBLIC },
    
    /* J2ME Bluetooth API (JSR-82) */
    { "javax/bluetooth/LocalDevice", "java/lang/Object", ACC_PUBLIC },
    { "javax/bluetooth/RemoteDevice", "java/lang/Object", ACC_PUBLIC },
    { "javax/bluetooth/DeviceClass", "java/lang/Object", ACC_PUBLIC },
    { "javax/bluetooth/DiscoveryAgent", "java/lang/Object", ACC_PUBLIC },
    { "javax/bluetooth/DiscoveryListener", "java/lang/Object", ACC_PUBLIC | ACC_INTERFACE },
    { "javax/bluetooth/ServiceRecord", "java/lang/Object", ACC_PUBLIC | ACC_INTERFACE },
    { "javax/bluetooth/UUID", "java/lang/Object", ACC_PUBLIC },
    { "javax/bluetooth/BluetoothConnectionException", "java/io/IOException", ACC_PUBLIC },
    { "javax/bluetooth/L2CAPConnection", "javax/microedition/io/Connection", ACC_PUBLIC | ACC_INTERFACE },
    { "javax/bluetooth/L2CAPConnectionNotifier", "javax/microedition/io/Connection", ACC_PUBLIC | ACC_INTERFACE },
    
    /* J2ME Wireless Messaging API (JSR-120) */
    { "javax/wireless/messaging/Message", "java/lang/Object", ACC_PUBLIC | ACC_INTERFACE },
    { "javax/wireless/messaging/TextMessage", "javax/wireless/messaging/Message", ACC_PUBLIC | ACC_INTERFACE },
    { "javax/wireless/messaging/BinaryMessage", "javax/wireless/messaging/Message", ACC_PUBLIC | ACC_INTERFACE },
    { "javax/wireless/messaging/MessageConnection", "javax/microedition/io/Connection", ACC_PUBLIC | ACC_INTERFACE },
    { "javax/wireless/messaging/MessageListener", "java/lang/Object", ACC_PUBLIC | ACC_INTERFACE },
    
    /* Siemens/Nokia specific extensions */
    { "com/siemens/mp/lcdui/Image", "java/lang/Object", ACC_PUBLIC },
    { "com/siemens/mp/game/Light", "java/lang/Object", ACC_PUBLIC },
    { "com/siemens/mp/game/Vibrator", "java/lang/Object", ACC_PUBLIC },
    { "com/siemens/mp/io/Connection", "java/lang/Object", ACC_PUBLIC },
    
    /* Motorola extensions */
    { "com/motorola/phone/Phone", "java/lang/Object", ACC_PUBLIC },
    { "com/motorola/phone/PhoneDisplay", "java/lang/Object", ACC_PUBLIC },
    { "com/motorola/phone/Vibrator", "java/lang/Object", ACC_PUBLIC },
    
    /* Samsung extensions */
    { "com/samsung/util/AudioClip", "java/lang/Object", ACC_PUBLIC },
    { "com/samsung/util/Vibration", "java/lang/Object", ACC_PUBLIC },
    { "com/samsung/util/LCDLight", "java/lang/Object", ACC_PUBLIC },
    { "com/samsung/util/SM", "java/lang/Object", ACC_PUBLIC },
    { "com/samsung/util/SMS", "java/lang/Object", ACC_PUBLIC },
    
    /* Mascot Capsule Micro3D v3 */
    { "com/mascotcapsule/micro3d/v3/ActionTable", "java/lang/Object", ACC_PUBLIC },
    { "com/mascotcapsule/micro3d/v3/AffineTrans", "java/lang/Object", ACC_PUBLIC },
    { "com/mascotcapsule/micro3d/v3/Effect3D", "java/lang/Object", ACC_PUBLIC },
    { "com/mascotcapsule/micro3d/v3/Figure", "java/lang/Object", ACC_PUBLIC },
    { "com/mascotcapsule/micro3d/v3/FigureLayout", "java/lang/Object", ACC_PUBLIC },
    { "com/mascotcapsule/micro3d/v3/Graphics3D", "java/lang/Object", ACC_PUBLIC },
    { "com/mascotcapsule/micro3d/v3/Light", "java/lang/Object", ACC_PUBLIC },
    { "com/mascotcapsule/micro3d/v3/Texture", "java/lang/Object", ACC_PUBLIC },
    { "com/mascotcapsule/micro3d/v3/Util3D", "java/lang/Object", ACC_PUBLIC },
    { "com/mascotcapsule/micro3d/v3/Vector3D", "java/lang/Object", ACC_PUBLIC },
    
    /* Siemens color_game (extends standard LCDUI game classes) */
    { "com/siemens/mp/color_game/GameCanvas", "javax/microedition/lcdui/game/GameCanvas", ACC_PUBLIC | ACC_ABSTRACT },
    { "com/siemens/mp/color_game/Layer", "javax/microedition/lcdui/game/Layer", ACC_PUBLIC | ACC_ABSTRACT },
    { "com/siemens/mp/color_game/LayerManager", "javax/microedition/lcdui/game/LayerManager", ACC_PUBLIC },
    { "com/siemens/mp/color_game/Sprite", "javax/microedition/lcdui/game/Sprite", ACC_PUBLIC },
    { "com/siemens/mp/color_game/TiledLayer", "javax/microedition/lcdui/game/TiledLayer", ACC_PUBLIC },
    
    /* Siemens additional game APIs */
    { "com/siemens/mp/game/ExtendedImage", "com/siemens/mp/misc/NativeMem", ACC_PUBLIC },
    { "com/siemens/mp/game/GraphicObject", "java/lang/Object", ACC_PUBLIC },
    { "com/siemens/mp/game/GraphicObjectManager", "com/siemens/mp/misc/NativeMem", ACC_PUBLIC },
    { "com/siemens/mp/game/Melody", "java/lang/Object", ACC_PUBLIC },
    { "com/siemens/mp/game/MelodyComposer", "java/lang/Object", ACC_PUBLIC },
    { "com/siemens/mp/game/Sound", "java/lang/Object", ACC_PUBLIC },
    { "com/siemens/mp/game/Sprite", "java/lang/Object", ACC_PUBLIC },
    { "com/siemens/mp/misc/NativeMem", "java/lang/Object", ACC_PUBLIC },
    
    { NULL, NULL, 0 }
};

/* Helper to ensure constant pool has space - only for stub classes during creation */
static void ensure_cp_capacity(JavaClass* clazz, size_t needed) {
    if (!clazz->constant_pool) {
        clazz->constant_pool_capacity = 64;
        clazz->constant_pool = (ConstantPoolEntry*)calloc(clazz->constant_pool_capacity, sizeof(ConstantPoolEntry));
        clazz->constant_pool_count = 1;  /* Index 0 unused */
    }
    
    if (clazz->constant_pool_count + needed >= clazz->constant_pool_capacity) {
        /* For stub classes, pre-allocate enough space */
        size_t new_cap = clazz->constant_pool_capacity + needed + 32;
        ConstantPoolEntry* new_pool = (ConstantPoolEntry*)calloc(new_cap, sizeof(ConstantPoolEntry));
        if (!new_pool) return;  /* Out of memory */
        
        /* Copy old entries */
        memcpy(new_pool, clazz->constant_pool, 
               clazz->constant_pool_count * sizeof(ConstantPoolEntry));
        
        free(clazz->constant_pool);
        clazz->constant_pool = new_pool;
        clazz->constant_pool_capacity = new_cap;

        /* v34.35: the CP grew — CP-index inline caches (opcodes.c) were
         * sized to the old constant_pool_count; drop them so they are
         * re-allocated at the new size. (Incomplete types: free() only.) */
        free(clazz->field_cache);
        free(clazz->invoke_cache);
        free(clazz->static_field_cache);
        /* v34.59 PERF: GC mark-layout cache (пересоздаётся лениво) */
        free(clazz->gc_ref_slots);
        clazz->gc_ref_slots = NULL;
        clazz->gc_ref_slot_count = 0;
        clazz->gc_kind = 0;
        clazz->field_cache = NULL;
        clazz->invoke_cache = NULL;
        clazz->static_field_cache = NULL;
    }
}

/* Create a minimal constant pool entry for UTF8 */
static uint16_t add_utf8(JavaClass* clazz, const char* str) {
    if (!str) return 0;
    
    size_t len = strlen(str);
    
    /* Check if already exists */
    for (uint16_t i = 1; i < clazz->constant_pool_count; i++) {
        if (clazz->constant_pool[i].tag == CONSTANT_Utf8 &&
            clazz->constant_pool[i].info.utf8.length == len &&
            clazz->constant_pool[i].info.utf8.bytes &&
            memcmp(clazz->constant_pool[i].info.utf8.bytes, str, len) == 0) {
            return i;
        }
    }
    
    ensure_cp_capacity(clazz, 1);
    
    uint16_t index = clazz->constant_pool_count++;
    clazz->constant_pool[index].tag = CONSTANT_Utf8;
    clazz->constant_pool[index].info.utf8.length = (uint16_t)len;
    clazz->constant_pool[index].info.utf8.bytes = strdup(str);
    
    return index;
}

/* Create a minimal constant pool entry for Class */
static uint16_t add_class_ref(JavaClass* clazz, const char* name) {
    if (!name) return 0;
    
    uint16_t name_index = add_utf8(clazz, name);
    
    /* Check if already exists */
    for (uint16_t i = 1; i < clazz->constant_pool_count; i++) {
        if (clazz->constant_pool[i].tag == CONSTANT_Class &&
            clazz->constant_pool[i].info.class_info.name_index == name_index) {
            return i;
        }
    }
    
    ensure_cp_capacity(clazz, 1);
    
    uint16_t index = clazz->constant_pool_count++;
    clazz->constant_pool[index].tag = CONSTANT_Class;
    clazz->constant_pool[index].info.class_info.name_index = name_index;
    
    return index;
}

/* Create a NameAndType constant */
static uint16_t add_name_and_type(JavaClass* clazz, const char* name, const char* descriptor) {
    if (!name || !descriptor) return 0;
    
    uint16_t name_index = add_utf8(clazz, name);
    uint16_t descriptor_index = add_utf8(clazz, descriptor);
    
    ensure_cp_capacity(clazz, 1);
    
    uint16_t index = clazz->constant_pool_count++;
    clazz->constant_pool[index].tag = CONSTANT_NameAndType;
    clazz->constant_pool[index].info.name_and_type.name_index = name_index;
    clazz->constant_pool[index].info.name_and_type.descriptor_index = descriptor_index;
    
    return index;
}

/* Create a Methodref constant */
static uint16_t add_method_ref(JavaClass* clazz, const char* class_name,
                               const char* name, const char* descriptor) {
    if (!class_name || !name || !descriptor) return 0;
    
    uint16_t class_index = add_class_ref(clazz, class_name);
    uint16_t name_and_type_index = add_name_and_type(clazz, name, descriptor);
    
    ensure_cp_capacity(clazz, 1);
    
    uint16_t index = clazz->constant_pool_count++;
    clazz->constant_pool[index].tag = CONSTANT_Methodref;
    clazz->constant_pool[index].info.ref_info.class_index = class_index;
    clazz->constant_pool[index].info.ref_info.name_and_type_index = name_and_type_index;
    
    return index;
}

/* Create a Fieldref constant */
static uint16_t add_field_ref(JavaClass* clazz, const char* class_name,
                              const char* name, const char* descriptor) {
    if (!class_name || !name || !descriptor) return 0;
    
    uint16_t class_index = add_class_ref(clazz, class_name);
    uint16_t name_and_type_index = add_name_and_type(clazz, name, descriptor);
    
    ensure_cp_capacity(clazz, 1);
    
    uint16_t index = clazz->constant_pool_count++;
    clazz->constant_pool[index].tag = CONSTANT_Fieldref;
    clazz->constant_pool[index].info.ref_info.class_index = class_index;
    clazz->constant_pool[index].info.ref_info.name_and_type_index = name_and_type_index;
    
    return index;
}

/* Ensure methods array has space */
static void ensure_methods_capacity(JavaClass* clazz) {
    if (!clazz->methods) {
        clazz->methods_capacity = 8;
        clazz->methods = (JavaMethod*)calloc(clazz->methods_capacity, sizeof(JavaMethod));
        clazz->methods_count = 0;
    }
    
    if (clazz->methods_count >= clazz->methods_capacity) {
        int new_cap = clazz->methods_capacity * 2;
        JavaMethod* new_methods = (JavaMethod*)calloc(new_cap, sizeof(JavaMethod));
        if (!new_methods) return;  /* Out of memory */
        
        /* Copy old entries */
        memcpy(new_methods, clazz->methods, 
               clazz->methods_count * sizeof(JavaMethod));
        
        free(clazz->methods);
        clazz->methods = new_methods;
        clazz->methods_capacity = new_cap;
        
        /* FIX (audit S-10, v18): all JavaMethod* previously handed out for this
         * class point into the freed array. Flush the method cache so no
         * dangling pointer is ever executed. */
        method_cache_flush();
        /* v34.35: the CP inline caches (opcodes.c InvokeRefCache) hold raw
         * JavaMethod* too — bump the generation so they self-invalidate. */
        {
            extern uint16_t g_vm_cache_gen;
            g_vm_cache_gen++;
        }
    }
}

/* Create a stub method - returns index in methods array */
static int create_stub_method(JavaClass* clazz, const char* name,
                              const char* descriptor, uint16_t access_flags,
                              const uint8_t* code, uint32_t code_len) {
    if (!name || !descriptor) return -1;
    
    ensure_methods_capacity(clazz);
    
    int idx = clazz->methods_count++;
    JavaMethod* method = &clazz->methods[idx];
    
    memset(method, 0, sizeof(JavaMethod));
    
    method->name = strdup(name);
    method->descriptor = strdup(descriptor);
    method->name_index = add_utf8(clazz, name);
    method->descriptor_index = add_utf8(clazz, descriptor);
    method->access_flags = access_flags;
    method->attributes_count = 0;
    method->clazz = clazz;
    /* FIX: sentinel "-1 = not yet computed" for the arg-count cache;
     * memset() above zeroes it which falsely means "0 arguments". */
    method->cached_arg_count = -1;
    
    /* Set is_native flag if ACC_NATIVE is set */
    method->is_native = (access_flags & ACC_NATIVE) ? 1 : 0;
    
    /* Create Code attribute if not abstract/native and code provided */
    if (!(access_flags & (ACC_ABSTRACT | ACC_NATIVE))) {
        /* === FIX: Calculate max_locals correctly === */
        int arg_slots = count_args(descriptor);
        int is_static = (access_flags & ACC_STATIC) != 0;
        
        /* Instance methods need 'this' + args. Static methods need just args. */
        method->code.max_locals = arg_slots + (is_static ? 0 : 1);
        
        method->code.max_stack = 2; /* Simple stub code usually doesn't need deep stack */
        method->code.code_length = code_len;
        method->code.code = (uint8_t*)malloc(code_len);
        if (method->code.code) {
            memcpy(method->code.code, code, code_len);
        }
    }
    
    return idx;
}

/* Create a stub class */
/* ==== v20 (P0-4 / FIX-20g restore): static final constants for stub classes ====
 * Platform stubs have no <clinit> and (as classfile stubs) no ConstantValue
 * attribute parsing, so `getstatic Math.PI`, `Integer.MAX_VALUE`,
 * `AnimationTrack.TRANSLATION`, `CompositingMode.REPLACE` ... all returned 0.
 * These helpers materialize them as real static slots at stub creation. */
static void stub_add_static_const(JavaClass* clazz, const char* name,
                                  const char* descriptor, JavaValue value) {
    if (!clazz || !name) return;
    if (!clazz->static_fields) {
        clazz->static_fields = (JavaStaticField*)calloc(16, sizeof(JavaStaticField));
        if (!clazz->static_fields) return;
        clazz->static_fields_count = 0;
        clazz->static_fields_capacity = 16;
    }
    if (clazz->static_fields_count >= clazz->static_fields_capacity) {
        int new_cap = clazz->static_fields_capacity * 2;
        JavaStaticField* nf = (JavaStaticField*)realloc(
            clazz->static_fields, (size_t)new_cap * sizeof(JavaStaticField));
        if (!nf) return;
        memset(nf + clazz->static_fields_capacity, 0,
               (size_t)(new_cap - clazz->static_fields_capacity) * sizeof(JavaStaticField));
        clazz->static_fields = nf;
        clazz->static_fields_capacity = new_cap;
    }
    /* Do not duplicate (idempotent for repeated init paths). */
    for (int i = 0; i < clazz->static_fields_count; i++) {
        if (clazz->static_fields[i].name && strcmp(clazz->static_fields[i].name, name) == 0 &&
            clazz->static_fields[i].descriptor && strcmp(clazz->static_fields[i].descriptor, descriptor) == 0) {
            return;
        }
    }
    JavaStaticField* sf = &clazz->static_fields[clazz->static_fields_count++];
    sf->name = strdup(name);
    sf->descriptor = strdup(descriptor);
    sf->value = value;
}

static void stub_add_static_int_consts(JavaClass* clazz, const char* const* names, const jint* values, int count) {
    for (int i = 0; i < count; i++) {
        JavaValue v; memset(&v, 0, sizeof(v)); v.i = values[i];
        stub_add_static_const(clazz, names[i], "I", v);
    }
}

static JavaClass* create_stub_class(JVM* jvm, const char* name, const char* super_name, uint16_t access_flags) {
    (void)jvm;
    if (!name) return NULL;
    
    JavaClass* clazz = (JavaClass*)calloc(1, sizeof(JavaClass));
    if (!clazz) return NULL;
    
    clazz->class_name = strdup(name);
    clazz->access_flags = access_flags;
    clazz->major_version = 47; /* JDK 1.3 */
    clazz->minor_version = 0;
    clazz->magic = 0xCAFEBABE;
    
    /* Initialize constant pool with space check */
    ensure_cp_capacity(clazz, 4);  /* Pre-allocate space for basic entries */
    
    /* Add this class */
    clazz->this_class = add_class_ref(clazz, name);
    
    /* Add super class */
    if (super_name) {
        clazz->super_class_index = add_class_ref(clazz, super_name);
        clazz->super_class_name = strdup(super_name);
    }
    
    /* Allocate methods array */
    ensure_methods_capacity(clazz);
    
    /* 
     * Add default constructor.
     * Constructors are needed for:
     * 1. Concrete classes (normal instantiation).
     * 2. Abstract classes (subclasses call super()).
     * Constructors are NOT needed for:
     * 1. Interfaces.
     * 2. System classes that should only be created via native factories (Image, etc).
     */
    bool add_constructor = !(access_flags & ACC_INTERFACE);
    
    /* List of classes that must NOT have a public constructor generated.
     * v18 FIX: added java/util/Date and java/util/Random — their native
     * <init> handlers ARE registered (native_date_init[_millis],
     * native_random_init[_seed]); the generated generic ctor used to win
     * method resolution and silently skip them, leaving the internal
     * field at 0 (Date.getTime() == 0, Random(seed) == Random(0)). */
    if (strcmp(name, "javax/microedition/lcdui/Image") == 0 ||
        strcmp(name, "javax/microedition/lcdui/Graphics") == 0 ||
        strcmp(name, "javax/microedition/lcdui/Font") == 0 ||
        strcmp(name, "javax/microedition/lcdui/Display") == 0 ||
        strcmp(name, "java/util/Date") == 0 ||
        strcmp(name, "java/util/Random") == 0 ||
        /* v34.3: new String() must reach native_string_init (generic
         * bytecode ctor left the value field NULL). */
        strcmp(name, "java/lang/String") == 0) {
        add_constructor = false;
    }
    
    /* CRITICAL: Display class needs native method stubs */
    if (strcmp(name, "javax/microedition/lcdui/Display") == 0) {
        /* Native methods don't need code - they're handled by native handlers */
        create_stub_method(clazz, "getDisplay", "(Ljavax/microedition/midlet/MIDlet;)Ljavax/microedition/lcdui/Display;", 
                          ACC_PUBLIC | ACC_STATIC | ACC_NATIVE, NULL, 0);
        create_stub_method(clazz, "setCurrent", "(Ljavax/microedition/lcdui/Displayable;)V", 
                          ACC_PUBLIC | ACC_NATIVE, NULL, 0);
        create_stub_method(clazz, "getCurrent", "()Ljavax/microedition/lcdui/Displayable;", 
                          ACC_PUBLIC | ACC_NATIVE, NULL, 0);
        create_stub_method(clazz, "callSerially", "(Ljava/lang/Runnable;)V", 
                          ACC_PUBLIC | ACC_NATIVE, NULL, 0);
        create_stub_method(clazz, "getWidth", "()I", 
                          ACC_PUBLIC | ACC_NATIVE, NULL, 0);
        create_stub_method(clazz, "getHeight", "()I", 
                          ACC_PUBLIC | ACC_NATIVE, NULL, 0);
        create_stub_method(clazz, "isColor", "()Z", 
                          ACC_PUBLIC | ACC_NATIVE, NULL, 0);
        create_stub_method(clazz, "numColors", "()I", 
                          ACC_PUBLIC | ACC_NATIVE, NULL, 0);
        create_stub_method(clazz, "numAlphaLevels", "()I", 
                          ACC_PUBLIC | ACC_NATIVE, NULL, 0);
        create_stub_method(clazz, "vibrate", "(I)Z", 
                          ACC_PUBLIC | ACC_NATIVE, NULL, 0);
        create_stub_method(clazz, "flashBacklight", "(I)Z", 
                          ACC_PUBLIC | ACC_NATIVE, NULL, 0);
    }
    
    /* CRITICAL: javax/microedition/midlet/MIDlet needs lifecycle methods.
     * Without these stubs, games that extend MIDlet will fail with
     * "No startApp method" because jvm_resolve_method finds nothing
     * in the abstract superclass. These empty implementations allow
     * the MIDlet lifecycle to proceed normally. */
    if (strcmp(name, "javax/microedition/midlet/MIDlet") == 0) {
        /* startApp() - protected abstract in real MIDlet, empty default here */
        uint8_t return_void[1];
        return_void[0] = 0xB1; /* return */
        create_stub_method(clazz, "startApp", "()V",
                          ACC_PROTECTED, return_void, 1);
        /* pauseApp() - protected abstract in real MIDlet */
        create_stub_method(clazz, "pauseApp", "()V",
                          ACC_PROTECTED, return_void, 1);
        /* destroyApp(boolean) - protected abstract in real MIDlet */
        create_stub_method(clazz, "destroyApp", "(Z)V",
                          ACC_PROTECTED, return_void, 1);
        /* notifyDestroyed() - public, tells MIDP the MIDlet is done */
        create_stub_method(clazz, "notifyDestroyed", "()V",
                          ACC_PUBLIC | ACC_NATIVE, NULL, 0);
        /* notifyPaused() - public */
        create_stub_method(clazz, "notifyPaused", "()V",
                          ACC_PUBLIC | ACC_NATIVE, NULL, 0);
        /* getAppProperty(String) - public, returns app property from JAD */
        create_stub_method(clazz, "getAppProperty",
                          "(Ljava/lang/String;)Ljava/lang/String;",
                          ACC_PUBLIC | ACC_NATIVE, NULL, 0);
        /* getDisplay() - convenience for Display.getDisplay(this) */
        create_stub_method(clazz, "getDisplay",
                          "()Ljavax/microedition/lcdui/Display;",
                          ACC_PUBLIC | ACC_NATIVE, NULL, 0);
        /* resumeRequest() - public */
        create_stub_method(clazz, "resumeRequest", "()V",
                          ACC_PUBLIC | ACC_NATIVE, NULL, 0);
    }
    
    /* FIX: java/lang/String constructor variants are NOT defined here anymore.
     * Previously this block created bytecode "trampolines" that just called
     * super.<init>() and silently dropped all arguments, so strings built with
     * new String(char[])/(String)/(StringBuffer)... ended up empty (value=NULL),
     * corrupting any MIDlet config parsing and later producing NullPointerExceptions.
     *
     * Now these constructors resolve through the native registry instead:
     * jvm_resolve_method() fails -> op_invokespecial falls back to
     * native_find(class, "<init>", descriptor) -> native_string_init_* runs
     * with full argument semantics (see native.c registrations).
     * Only <init>()V stays defined by the generic add_constructor path above.
     */
    
    if (add_constructor) {
        /* 
         * CRITICAL FIX: If this is java/lang/Object, the constructor must not call super.
         * It should just return.
         */
        if (super_name == NULL) {
            /* java/lang/Object constructor: just return */
            uint8_t init_code[1];
            init_code[0] = 0xB1; /* return */
            create_stub_method(clazz, "<init>", "()V", ACC_PUBLIC, init_code, 1);
            
            /* toString() method for Object - returns getClass().getName() + "@" + hashCode() */
            /* Simplified: just return getClass().getName() to avoid complex code in stub */
            /* Code: aload_0, invokevirtual getClass(), invokevirtual getName(), areturn */
            /* Since this requires method refs, we use native implementation */
        } else {
            /* Subclass constructor: aload_0, invokespecial super.<init>, return */
            const char* super = super_name;
            uint16_t super_init_ref = add_method_ref(clazz, super, "<init>", "()V");
            
            uint8_t init_code[5];
            init_code[0] = 0x2A; /* aload_0 */
            init_code[1] = 0xB7; /* invokespecial */
            init_code[2] = (super_init_ref >> 8) & 0xFF;
            init_code[3] = super_init_ref & 0xFF;
            init_code[4] = 0xB1; /* return */
            
            create_stub_method(clazz, "<init>", "()V", ACC_PUBLIC, init_code, 5);
        }
        
        /* Special case for GameCanvas: it needs a constructor (boolean) */
        if (strcmp(name, "javax/microedition/lcdui/game/GameCanvas") == 0) {
            /* public GameCanvas(boolean suppressKeyEvents) */
            /* Code: aload_0, invokespecial super.<init>(), return */
            /* The boolean argument is simply ignored in the stub. */
            
            uint16_t super_init_ref = add_method_ref(clazz, "javax/microedition/lcdui/Canvas", "<init>", "()V");
            
            uint8_t init_code[5];
            init_code[0] = 0x2A; /* aload_0 */
            init_code[1] = 0xB7; /* invokespecial */
            init_code[2] = (super_init_ref >> 8) & 0xFF;
            init_code[3] = super_init_ref & 0xFF;
            init_code[4] = 0xB1; /* return */
            
            /* Descriptor (Z)V means takes boolean, returns void */
            create_stub_method(clazz, "<init>", "(Z)V", ACC_PUBLIC, init_code, 5);
            
            /* ИСПРАВЛЕНО: Добавляем paint(Graphics g) - пустая реализация для super.paint() */
            uint8_t paint_code[1];
            paint_code[0] = 0xB1; /* return */
            create_stub_method(clazz, "paint", "(Ljavax/microedition/lcdui/Graphics;)V",
                              ACC_PUBLIC, paint_code, 1);
        }

        /* v34.3 FIX: the stub bytecode constructors that used to live here
         * only called super.<init>() and silently DROPPED their arguments.
         * Before v34 the execute.c stub shortcut dispatched them to the
         * registered native <init>; after the v34 narrowing ("stub shortcut
         * only for methods WITHOUT bytecode") the no-op bytecode won and the
         * native never ran (ByteArrayInputStream kept buf=NULL/count=0 ->
         * Manager.createPlayer threw MediaException; Alert lost title/text;
         * Sound/StringBuffer lost their data). All these constructors have
         * registered natives now - resolution reaches them through the
         * op_invokespecial constructor fallback (v18 FIX), so the block is
         * simply gone. See FIXES.txt v34.3. */

        /* ИСПРАВЛЕНО: Добавляем paint() для Canvas тоже */
        if (strcmp(name, "javax/microedition/lcdui/Canvas") == 0) {
            /* paint(Graphics g) - пустая реализация */
            uint8_t paint_code[1];
            paint_code[0] = 0xB1; /* return */
            create_stub_method(clazz, "paint", "(Ljavax/microedition/lcdui/Graphics;)V", 
                              ACC_PUBLIC, paint_code, 1);
            
            /* keyPressed(int) - пустая заглушка */
            create_stub_method(clazz, "keyPressed", "(I)V", ACC_PUBLIC, paint_code, 1);
            
            /* keyReleased(int) - пустая заглушка */
            create_stub_method(clazz, "keyReleased", "(I)V", ACC_PUBLIC, paint_code, 1);
            
            /* pointerPressed(int, int) - пустая заглушка */
            create_stub_method(clazz, "pointerPressed", "(II)V", ACC_PUBLIC, paint_code, 1);
            
            /* pointerReleased(int, int) - пустая заглушка */
            create_stub_method(clazz, "pointerReleased", "(II)V", ACC_PUBLIC, paint_code, 1);
            
            /* Native methods for Canvas */
            create_stub_method(clazz, "getWidth", "()I", ACC_PUBLIC | ACC_NATIVE, NULL, 0);
            create_stub_method(clazz, "getHeight", "()I", ACC_PUBLIC | ACC_NATIVE, NULL, 0);
            create_stub_method(clazz, "getGameAction", "(I)I", ACC_PUBLIC | ACC_NATIVE, NULL, 0);
            create_stub_method(clazz, "getKeyCode", "(I)I", ACC_PUBLIC | ACC_NATIVE, NULL, 0);
            create_stub_method(clazz, "getKeyName", "(I)Ljava/lang/String;", ACC_PUBLIC | ACC_NATIVE, NULL, 0);
            create_stub_method(clazz, "hasPointerEvents", "()Z", ACC_PUBLIC | ACC_NATIVE, NULL, 0);
            create_stub_method(clazz, "hasPointerMotionEvents", "()Z", ACC_PUBLIC | ACC_NATIVE, NULL, 0);
            create_stub_method(clazz, "hasRepeatEvents", "()Z", ACC_PUBLIC | ACC_NATIVE, NULL, 0);
            create_stub_method(clazz, "isDoubleBuffered", "()Z", ACC_PUBLIC | ACC_NATIVE, NULL, 0);
            create_stub_method(clazz, "repaint", "()V", ACC_PUBLIC | ACC_NATIVE, NULL, 0);
            create_stub_method(clazz, "repaint", "(IIII)V", ACC_PUBLIC | ACC_NATIVE, NULL, 0);
            create_stub_method(clazz, "serviceRepaints", "()V", ACC_PUBLIC | ACC_NATIVE, NULL, 0);
            create_stub_method(clazz, "setFullScreenMode", "(Z)V", ACC_PUBLIC | ACC_NATIVE, NULL, 0);
            create_stub_method(clazz, "isShown", "()Z", ACC_PUBLIC | ACC_NATIVE, NULL, 0);

            /* FIX-19o: MIDP-spec static constants. They were missing, so
             * getstatic Canvas.UP/DOWN/... returned 0 for ALL actions -
             * getKeyCode(0)==getKeyCode(0) collapsed UP/DOWN/LEFT/RIGHT/FIRE
             * into one code and broke game-action roundtrips in every game. */
            {
                #define CANVAS_CONST_COUNT 21
                static const struct { const char* name; int value; } canvas_consts[CANVAS_CONST_COUNT] = {
                    { "KEY_NUM0", 48 }, { "KEY_NUM1", 49 }, { "KEY_NUM2", 50 },
                    { "KEY_NUM3", 51 }, { "KEY_NUM4", 52 }, { "KEY_NUM5", 53 },
                    { "KEY_NUM6", 54 }, { "KEY_NUM7", 55 }, { "KEY_NUM8", 56 },
                    { "KEY_NUM9", 57 },
                    { "KEY_STAR", 42 }, { "KEY_POUND", 35 },
                    { "UP", 1 }, { "DOWN", 6 }, { "LEFT", 2 },
                    { "RIGHT", 5 }, { "FIRE", 8 },
                    { "GAME_A", 9 }, { "GAME_B", 10 },
                    { "GAME_C", 11 }, { "GAME_D", 12 },
                };
                clazz->static_fields = (JavaStaticField*)calloc(24, sizeof(JavaStaticField));
                clazz->static_fields_count = 0;
                clazz->static_fields_capacity = 24;
                if (clazz->static_fields) {
                    for (int ci = 0; ci < CANVAS_CONST_COUNT; ci++) {
                        JavaStaticField* sf = &clazz->static_fields[clazz->static_fields_count++];
                        sf->name = strdup(canvas_consts[ci].name);
                        sf->descriptor = strdup("I");
                        memset(&sf->value, 0, sizeof(JavaValue));
                        sf->value.i = canvas_consts[ci].value;
                    }
                }
                #undef CANVAS_CONST_COUNT
            }
        }

        /* ==== v20 (P0-4): java.lang static final constants on stubs ====
         * Math.PI/E, Integer.MIN_VALUE/MAX_VALUE, Boolean.TRUE/FALSE,
         * Double.NaN/... were all 0/null through getstatic (stubs have no
         * <clinit>). */
        if (strcmp(name, "java/lang/Math") == 0) {
            JavaValue v; memset(&v, 0, sizeof(v));
            v.d = 2.7182818284590452354;  stub_add_static_const(clazz, "E",  "D", v);
            v.d = 3.14159265358979323846; stub_add_static_const(clazz, "PI", "D", v);
        }
        else if (strcmp(name, "java/lang/Integer") == 0) {
            static const char* n[] = { "MIN_VALUE", "MAX_VALUE" };
            const jint vals[] = { INT32_MIN, INT32_MAX };
            stub_add_static_int_consts(clazz, n, vals, 2);
        }
        else if (strcmp(name, "java/lang/Long") == 0) {
            /* 64-bit values: registered directly, not via the int-consts helper */
            JavaValue v; memset(&v, 0, sizeof(v));
            v.j = INT64_MIN; stub_add_static_const(clazz, "MIN_VALUE", "J", v);
            v.j = INT64_MAX; stub_add_static_const(clazz, "MAX_VALUE", "J", v);
        }
        else if (strcmp(name, "java/lang/Short") == 0) {
            const char* n[] = { "MIN_VALUE", "MAX_VALUE" };
            const jint vals[] = { -32768, 32767 };
            stub_add_static_int_consts(clazz, n, vals, 2);
        }
        else if (strcmp(name, "java/lang/Byte") == 0) {
            const char* n[] = { "MIN_VALUE", "MAX_VALUE" };
            const jint vals[] = { -128, 127 };
            stub_add_static_int_consts(clazz, n, vals, 2);
        }
        else if (strcmp(name, "java/lang/Character") == 0) {
            const char* n[] = { "MIN_RADIX", "MAX_RADIX" };
            const jint vals[] = { 2, 36 };
            stub_add_static_int_consts(clazz, n, vals, 2);
            JavaValue v; memset(&v, 0, sizeof(v));
            v.i = 0;     stub_add_static_const(clazz, "MIN_VALUE", "C", v);
            v.i = 65535; stub_add_static_const(clazz, "MAX_VALUE", "C", v);
        }
        else if (strcmp(name, "java/lang/Float") == 0) {
            JavaValue v; memset(&v, 0, sizeof(v));
            v.f = 1.4e-45f;      stub_add_static_const(clazz, "MIN_VALUE", "F", v);
            v.f = 3.4028234663852886e+38f; stub_add_static_const(clazz, "MAX_VALUE", "F", v);
            v.f = INFINITY;      stub_add_static_const(clazz, "POSITIVE_INFINITY", "F", v);
            v.f = -INFINITY;     stub_add_static_const(clazz, "NEGATIVE_INFINITY", "F", v);
            v.f = NAN;           stub_add_static_const(clazz, "NaN", "F", v);
        }
        else if (strcmp(name, "java/lang/Double") == 0) {
            JavaValue v; memset(&v, 0, sizeof(v));
            v.d = 4.9e-324;      stub_add_static_const(clazz, "MIN_VALUE", "D", v);
            v.d = 1.7976931348623157e+308; stub_add_static_const(clazz, "MAX_VALUE", "D", v);
            v.d = INFINITY;      stub_add_static_const(clazz, "POSITIVE_INFINITY", "D", v);
            v.d = -INFINITY;     stub_add_static_const(clazz, "NEGATIVE_INFINITY", "D", v);
            v.d = NAN;           stub_add_static_const(clazz, "NaN", "D", v);
        }
        else if (strcmp(name, "java/lang/Boolean") == 0) {
            /* v20: reserve TRUE/FALSE slots with the OBJECT descriptor.
             * The instances are materialized at the END of
             * init_stub_classes() — here the "value" instance field does not
             * exist yet, so allocating now would produce objects without
             * room for it (the v15 undersized-stub lesson). */
            JavaValue rt; memset(&rt, 0, sizeof(rt)); rt.ref = NULL;
            stub_add_static_const(clazz, "TRUE", "Ljava/lang/Boolean;", rt);
            JavaValue rf; memset(&rf, 0, sizeof(rf)); rf.ref = NULL;
            stub_add_static_const(clazz, "FALSE", "Ljava/lang/Boolean;", rf);
        }

        /* ==== v20 (FIX-20g restore): JSR-184 static final constants ====
         * `new AnimationTrack(seq, AnimationTrack.TRANSLATION)` passed
         * propertyId=0 without these, silently killing animation tracks
         * created through the API. Values match JSR-184 and the internal
         * M3G_ANIM_* evaluation table. */
        if (strcmp(name, "javax/microedition/m3g/AnimationTrack") == 0) {
            static const char* n[] = {
                "ALPHA", "AMBIENT_COLOR", "COLOR", "CROP", "DENSITY",
                "DIFFUSE_COLOR", "EMISSIVE_COLOR", "FAR_DISTANCE",
                "FIELD_OF_VIEW", "INTENSITY", "MORPH_WEIGHTS",
                "NEAR_DISTANCE", "ORIENTATION", "PICKABILITY", "SCALE",
                "SHININESS", "SPECULAR_COLOR", "SPOT_ANGLE",
                "SPOT_EXPONENT", "TRANSLATION", "VISIBILITY" };
            static const jint v[] = {
                256, 257, 258, 259, 260, 261, 262, 263, 264, 265, 266,
                267, 268, 269, 270, 271, 272, 273, 274, 275, 276 };
            stub_add_static_int_consts(clazz, n, v, 21);
        }
        else if (strcmp(name, "javax/microedition/m3g/KeyframeSequence") == 0) {
            /* v23: JSR-184 javadoc literals — LINEAR=176, SLERP=177, SPLINE=178,
             * SQUAD=179, STEP=180. Games that hardcode the javadoc values no
             * longer hit off-by-one; the evaluator normalizes both encodings
             * (0..3 legacy kept working via m3g_normalize_interpolation).
             * v31 FIX: CONSTANT was 180 (= STEP, collision) and LOOP was
             * MISSING entirely — getstatic KeyframeSequence.LOOP returned 0,
             * setRepeatMode(0) never matched M3G_REPEAT_LOOP(193), so every
             * LOOPING keyframe animation clamped to an endpoint and scenes
             * froze at a single pose (M3GTest scenes 5 & 7 static). Real
             * JSR-184 values: CONSTANT=192, LOOP=193 (same bytes the M3G
             * file format stores). */
            static const char* n[] = { "LINEAR", "SLERP", "SPLINE", "SQUAD", "STEP",
                                       "CONSTANT", "LOOP" };
            static const jint v[] = { 176, 177, 178, 179, 180, 192, 193 };
            stub_add_static_int_consts(clazz, n, v, 7);
        }
        else if (strcmp(name, "javax/microedition/m3g/CompositingMode") == 0) {
            static const char* n[] = { "ALPHA", "ALPHA_ADD", "MODULATE", "MODULATE_X2", "REPLACE" };
            static const jint v[] = { 64, 65, 66, 67, 68 };
            stub_add_static_int_consts(clazz, n, v, 5);
        }
        else if (strcmp(name, "javax/microedition/m3g/PolygonMode") == 0) {
            /* v31 FIX: WINDING_CCW/WINDING_CW were SWAPPED (CW=168, CCW=169).
             * Real JSR-184: WINDING_CCW=168, WINDING_CW=169 (matches the
             * CULL_* block order used by every reference stub). */
            static const char* n[] = { "CULL_BACK", "CULL_FRONT", "CULL_NONE",
                                       "SHADE_FLAT", "SHADE_SMOOTH",
                                       "WINDING_CCW", "WINDING_CW" };
            static const jint v[] = { 160, 161, 162, 164, 165, 168, 169 };
            stub_add_static_int_consts(clazz, n, v, 7);
        }
        else if (strcmp(name, "javax/microedition/m3g/Texture2D") == 0) {
            /* v31 FIX: added the five FUNC_* blending constants (real JSR-184
             * values: FUNC_ADD=224, FUNC_BLEND=225, FUNC_DECAL=226,
             * FUNC_MODULATE=227, FUNC_REPLACE=228). They were MISSING from
             * the stub — getstatic Texture2D.FUNC_REPLACE returned 0, so
             * setBlending(0) landed on the "unset" default and every texture
             * blending demonstration collapsed to MODULATE (M3GTest scene 4
             * showed no per-quad blending difference). The renderer already
             * maps the 224..228 block (it was labeled "Nokia-era" but it is
             * the actual JSR-184 constant block). */
            static const char* n[] = { "FILTER_BASE_LEVEL", "FILTER_LINEAR", "FILTER_NEAREST",
                                       "FILTER_TRILINEAR", "WRAP_CLAMP", "WRAP_REPEAT",
                                       "FUNC_ADD", "FUNC_BLEND", "FUNC_DECAL",
                                       "FUNC_MODULATE", "FUNC_REPLACE" };
            static const jint v[] = { 208, 209, 210, 211, 240, 241,
                                      224, 225, 226, 227, 228 };
            stub_add_static_int_consts(clazz, n, v, 11);
        }
        else if (strcmp(name, "javax/microedition/m3g/Camera") == 0) {
            static const char* n[] = { "GENERIC", "PARALLEL", "PERSPECTIVE" };
            static const jint v[] = { 0, 1, 2 };
            stub_add_static_int_consts(clazz, n, v, 3);
        }
        else if (strcmp(name, "javax/microedition/m3g/Light") == 0) {
            /* v31 FIX: the stub block 0..3 is kept as the internal encoding
             * (m3g_light_mode_to_type accepts both), but the real JSR-184
             * API values are 128..131 — midlets that inline the constants
             * (compiled against the real API's `static final` fields) pass
             * 128..131 to setMode, which the old stub never provided. */
            static const char* n[] = { "AMBIENT", "DIRECTIONAL", "OMNI", "SPOT" };
            static const jint v[] = { 128, 129, 130, 131 };
            stub_add_static_int_consts(clazz, n, v, 4);
        }
        else if (strcmp(name, "javax/microedition/m3g/Material") == 0) {
            /* v31 CRITICAL FIX: the Material stub registered NO constants —
             * getstatic Material.AMBIENT (etc.) returned 0, and
             * Material.setColor(0, argb) matched no component, so EVERY
             * material kept its zero-initialized fields: ambient/diffuse/
             * specular/emissive all 0x00000000 (m3gtest scene 3 rendered
             * black). Real JSR-184 values: AMBIENT=32, DIFFUSE=33,
             * EMISSIVE=34, SPECULAR=35 (setColor takes an enum, not a
             * bitmask — the native now accepts both encodings). */
            static const char* n[] = { "AMBIENT", "DIFFUSE", "EMISSIVE", "SPECULAR" };
            static const jint v[] = { 32, 33, 34, 35 };
            stub_add_static_int_consts(clazz, n, v, 4);
        }
        else if (strcmp(name, "javax/microedition/m3g/Fog") == 0) {
            static const char* n[] = { "LINEAR", "EXPONENTIAL" };
            static const jint v[] = { 80, 81 };
            stub_add_static_int_consts(clazz, n, v, 2);
        }
        else if (strcmp(name, "javax/microedition/m3g/Image2D") == 0) {
            static const char* n[] = { "FORMAT_ALPHA", "FORMAT_LUMINANCE", "FORMAT_LUMINANCE_ALPHA",
                                       "FORMAT_RGB", "FORMAT_RGBA" };
            static const jint v[] = { 96, 97, 98, 99, 100 };
            stub_add_static_int_consts(clazz, n, v, 5);
        }
        
        /* Displayable native methods */
        if (strcmp(name, "javax/microedition/lcdui/Displayable") == 0) {
            create_stub_method(clazz, "getWidth", "()I", ACC_PUBLIC | ACC_NATIVE, NULL, 0);
            create_stub_method(clazz, "getHeight", "()I", ACC_PUBLIC | ACC_NATIVE, NULL, 0);
            create_stub_method(clazz, "isShown", "()Z", ACC_PUBLIC | ACC_NATIVE, NULL, 0);
            create_stub_method(clazz, "repaint", "()V", ACC_PUBLIC | ACC_NATIVE, NULL, 0);
            create_stub_method(clazz, "repaint", "(IIII)V", ACC_PUBLIC | ACC_NATIVE, NULL, 0);
            create_stub_method(clazz, "addCommand", "(Ljavax/microedition/lcdui/Command;)V", ACC_PUBLIC | ACC_NATIVE, NULL, 0);
            create_stub_method(clazz, "removeCommand", "(Ljavax/microedition/lcdui/Command;)V", ACC_PUBLIC | ACC_NATIVE, NULL, 0);
            create_stub_method(clazz, "setCommandListener", "(Ljavax/microedition/lcdui/CommandListener;)V", ACC_PUBLIC | ACC_NATIVE, NULL, 0);
        }
        
        /* Special case for List: needs constructor (String, int, String[], Image[]) */
        if (strcmp(name, "javax/microedition/lcdui/List") == 0) {
            /* public List(String title, int listType, String[] stringElements, Image[] imageElements) */
            /* Code: aload_0, invokespecial super.<init>(), return */
            /* The actual initialization is handled by native handler */
            
            uint16_t super_init_ref = add_method_ref(clazz, "javax/microedition/lcdui/Screen", "<init>", "()V");
            
            uint8_t init_code[5];
            init_code[0] = 0x2A; /* aload_0 */
            init_code[1] = 0xB7; /* invokespecial */
            init_code[2] = (super_init_ref >> 8) & 0xFF;
            init_code[3] = super_init_ref & 0xFF;
            init_code[4] = 0xB1; /* return */
            
            /* Constructor: List(String title, int listType) */
            create_stub_method(clazz, "<init>", "(Ljava/lang/String;I)V", ACC_PUBLIC, init_code, 5);
            
            /* Constructor: List(String title, int listType, String[] stringElements, Image[] imageElements) */
            create_stub_method(clazz, "<init>", "(Ljava/lang/String;I[Ljava/lang/String;[Ljavax/microedition/lcdui/Image;)V", ACC_PUBLIC, init_code, 5);
            
            /* Native methods for List */
            create_stub_method(clazz, "append", "(Ljava/lang/String;Ljavax/microedition/lcdui/Image;)I", ACC_PUBLIC | ACC_NATIVE, NULL, 0);
            create_stub_method(clazz, "getSelectedIndex", "()I", ACC_PUBLIC | ACC_NATIVE, NULL, 0);
            create_stub_method(clazz, "setSelectedIndex", "(IZ)V", ACC_PUBLIC | ACC_NATIVE, NULL, 0);
            create_stub_method(clazz, "getString", "(I)Ljava/lang/String;", ACC_PUBLIC | ACC_NATIVE, NULL, 0);
            create_stub_method(clazz, "size", "()I", ACC_PUBLIC | ACC_NATIVE, NULL, 0);
        }
        
        /* Special case for Form: needs constructor (String) and constructor (String, Item[]) */
        if (strcmp(name, "javax/microedition/lcdui/Form") == 0) {
            uint16_t super_init_ref = add_method_ref(clazz, "javax/microedition/lcdui/Screen", "<init>", "()V");
            
            uint8_t init_code[5];
            init_code[0] = 0x2A; /* aload_0 */
            init_code[1] = 0xB7; /* invokespecial */
            init_code[2] = (super_init_ref >> 8) & 0xFF;
            init_code[3] = super_init_ref & 0xFF;
            init_code[4] = 0xB1; /* return */
            
            /* Constructor: Form(String title) */
            create_stub_method(clazz, "<init>", "(Ljava/lang/String;)V", ACC_PUBLIC, init_code, 5);
            
            /* Constructor: Form(String title, Item[] items) */
            create_stub_method(clazz, "<init>", "(Ljava/lang/String;[Ljavax/microedition/lcdui/Item;)V", ACC_PUBLIC, init_code, 5);
            
            /* Native methods for Form */
            create_stub_method(clazz, "append", "(Ljavax/microedition/lcdui/Item;)I", ACC_PUBLIC | ACC_NATIVE, NULL, 0);
            create_stub_method(clazz, "delete", "(I)V", ACC_PUBLIC | ACC_NATIVE, NULL, 0);
            create_stub_method(clazz, "size", "()I", ACC_PUBLIC | ACC_NATIVE, NULL, 0);
            create_stub_method(clazz, "get", "(I)Ljavax/microedition/lcdui/Item;", ACC_PUBLIC | ACC_NATIVE, NULL, 0);
            create_stub_method(clazz, "getTitle", "()Ljava/lang/String;", ACC_PUBLIC | ACC_NATIVE, NULL, 0);
            create_stub_method(clazz, "setTitle", "(Ljava/lang/String;)V", ACC_PUBLIC | ACC_NATIVE, NULL, 0);
            create_stub_method(clazz, "setItemStateListener", "(Ljavax/microedition/lcdui/ItemStateListener;)V", ACC_PUBLIC | ACC_NATIVE, NULL, 0);
        }
        
        /* Special case for Command: needs constructor (String, int, int) */
        if (strcmp(name, "javax/microedition/lcdui/Command") == 0) {
            /* public Command(String label, int type, int priority) */
            /* Code: aload_0, invokespecial super.<init>(), return */
            /* The actual initialization is handled by native handler */
            
            uint16_t super_init_ref = add_method_ref(clazz, "java/lang/Object", "<init>", "()V");
            
            uint8_t init_code[5];
            init_code[0] = 0x2A; /* aload_0 */
            init_code[1] = 0xB7; /* invokespecial */
            init_code[2] = (super_init_ref >> 8) & 0xFF;
            init_code[3] = super_init_ref & 0xFF;
            init_code[4] = 0xB1; /* return */
            
            /* Constructor: Command(String label, int type, int priority) */
            create_stub_method(clazz, "<init>", "(Ljava/lang/String;II)V", ACC_PUBLIC, init_code, 5);
        }
        
        /* Special case for M3G Mesh: needs constructor (VertexBuffer, IndexBuffer, Appearance) */
        if (strcmp(name, "javax/microedition/m3g/Mesh") == 0) {
            /* public Mesh(VertexBuffer vertices, IndexBuffer indices, Appearance appearance) */
            /* Code: aload_0, invokespecial super.<init>(), return */
            /* The actual initialization is handled by native handler */
            
            uint16_t super_init_ref = add_method_ref(clazz, "javax/microedition/m3g/Node", "<init>", "()V");
            
            uint8_t init_code[5];
            init_code[0] = 0x2A; /* aload_0 */
            init_code[1] = 0xB7; /* invokespecial */
            init_code[2] = (super_init_ref >> 8) & 0xFF;
            init_code[3] = super_init_ref & 0xFF;
            init_code[4] = 0xB1; /* return */
            
            /* Constructor: Mesh(VertexBuffer, IndexBuffer, Appearance) */
            create_stub_method(clazz, "<init>", "(Ljavax/microedition/m3g/VertexBuffer;Ljavax/microedition/m3g/IndexBuffer;Ljavax/microedition/m3g/Appearance;)V", ACC_PUBLIC, init_code, 5);
        }
        
        /* Special case for M3G VertexArray: needs constructor (int, int, int) */
        if (strcmp(name, "javax/microedition/m3g/VertexArray") == 0) {
            uint16_t super_init_ref = add_method_ref(clazz, "java/lang/Object", "<init>", "()V");
            
            uint8_t init_code[5];
            init_code[0] = 0x2A; /* aload_0 */
            init_code[1] = 0xB7; /* invokespecial */
            init_code[2] = (super_init_ref >> 8) & 0xFF;
            init_code[3] = super_init_ref & 0xFF;
            init_code[4] = 0xB1; /* return */
            
            /* Constructor: VertexArray(int numVertices, int numComponents, int componentSize) */
            create_stub_method(clazz, "<init>", "(III)V", ACC_PUBLIC, init_code, 5);
        }
        
        /* Special case for M3G TriangleStripArray: needs constructor (int, int[]) */
        if (strcmp(name, "javax/microedition/m3g/TriangleStripArray") == 0) {
            uint16_t super_init_ref = add_method_ref(clazz, "javax/microedition/m3g/IndexBuffer", "<init>", "()V");
            
            uint8_t init_code[5];
            init_code[0] = 0x2A; /* aload_0 */
            init_code[1] = 0xB7; /* invokespecial */
            init_code[2] = (super_init_ref >> 8) & 0xFF;
            init_code[3] = super_init_ref & 0xFF;
            init_code[4] = 0xB1; /* return */
            
            /* Constructor: TriangleStripArray(int firstIndex, int[] stripLengths) */
            create_stub_method(clazz, "<init>", "(I[I)V", ACC_PUBLIC, init_code, 5);
        }
        
        /* Special case for M3G CompositingMode: needs default constructor */
        if (strcmp(name, "javax/microedition/m3g/CompositingMode") == 0) {
            uint16_t super_init_ref = add_method_ref(clazz, "java/lang/Object", "<init>", "()V");
            
            uint8_t init_code[5];
            init_code[0] = 0x2A; /* aload_0 */
            init_code[1] = 0xB7; /* invokespecial */
            init_code[2] = (super_init_ref >> 8) & 0xFF;
            init_code[3] = super_init_ref & 0xFF;
            init_code[4] = 0xB1; /* return */
            
            /* Constructor: CompositingMode() */
            create_stub_method(clazz, "<init>", "()V", ACC_PUBLIC, init_code, 5);
        }
        
        /* Special case for M3G Fog: needs default constructor */
        if (strcmp(name, "javax/microedition/m3g/Fog") == 0) {
            uint16_t super_init_ref = add_method_ref(clazz, "java/lang/Object", "<init>", "()V");
            
            uint8_t init_code[5];
            init_code[0] = 0x2A; /* aload_0 */
            init_code[1] = 0xB7; /* invokespecial */
            init_code[2] = (super_init_ref >> 8) & 0xFF;
            init_code[3] = super_init_ref & 0xFF;
            init_code[4] = 0xB1; /* return */
            
            /* Constructor: Fog() */
            create_stub_method(clazz, "<init>", "()V", ACC_PUBLIC, init_code, 5);
        }
        
        /* Special case for M3G Appearance: needs default constructor */
        if (strcmp(name, "javax/microedition/m3g/Appearance") == 0) {
            uint16_t super_init_ref = add_method_ref(clazz, "java/lang/Object", "<init>", "()V");
            
            uint8_t init_code[5];
            init_code[0] = 0x2A; /* aload_0 */
            init_code[1] = 0xB7; /* invokespecial */
            init_code[2] = (super_init_ref >> 8) & 0xFF;
            init_code[3] = super_init_ref & 0xFF;
            init_code[4] = 0xB1; /* return */
            
            /* Constructor: Appearance() */
            create_stub_method(clazz, "<init>", "()V", ACC_PUBLIC, init_code, 5);
        }
        
        /* Special case for M3G Camera: needs default constructor */
        if (strcmp(name, "javax/microedition/m3g/Camera") == 0) {
            uint16_t super_init_ref = add_method_ref(clazz, "javax/microedition/m3g/Node", "<init>", "()V");
            
            uint8_t init_code[5];
            init_code[0] = 0x2A; /* aload_0 */
            init_code[1] = 0xB7; /* invokespecial */
            init_code[2] = (super_init_ref >> 8) & 0xFF;
            init_code[3] = super_init_ref & 0xFF;
            init_code[4] = 0xB1; /* return */
            
            /* Constructor: Camera() */
            create_stub_method(clazz, "<init>", "()V", ACC_PUBLIC, init_code, 5);
        }
        
        /* Special case for M3G Light: needs default constructor */
        if (strcmp(name, "javax/microedition/m3g/Light") == 0) {
            uint16_t super_init_ref = add_method_ref(clazz, "javax/microedition/m3g/Node", "<init>", "()V");
            
            uint8_t init_code[5];
            init_code[0] = 0x2A; /* aload_0 */
            init_code[1] = 0xB7; /* invokespecial */
            init_code[2] = (super_init_ref >> 8) & 0xFF;
            init_code[3] = super_init_ref & 0xFF;
            init_code[4] = 0xB1; /* return */
            
            /* Constructor: Light() */
            create_stub_method(clazz, "<init>", "()V", ACC_PUBLIC, init_code, 5);
        }
        
        /* Special case for M3G Background: needs default constructor */
        if (strcmp(name, "javax/microedition/m3g/Background") == 0) {
            uint16_t super_init_ref = add_method_ref(clazz, "javax/microedition/m3g/Object3D", "<init>", "()V");
            
            uint8_t init_code[5];
            init_code[0] = 0x2A; /* aload_0 */
            init_code[1] = 0xB7; /* invokespecial */
            init_code[2] = (super_init_ref >> 8) & 0xFF;
            init_code[3] = super_init_ref & 0xFF;
            init_code[4] = 0xB1; /* return */
            
            /* Constructor: Background() */
            create_stub_method(clazz, "<init>", "()V", ACC_PUBLIC, init_code, 5);
        }
        
        /* Special case for M3G Material: needs default constructor */
        if (strcmp(name, "javax/microedition/m3g/Material") == 0) {
            uint16_t super_init_ref = add_method_ref(clazz, "java/lang/Object", "<init>", "()V");
            
            uint8_t init_code[5];
            init_code[0] = 0x2A; /* aload_0 */
            init_code[1] = 0xB7; /* invokespecial */
            init_code[2] = (super_init_ref >> 8) & 0xFF;
            init_code[3] = super_init_ref & 0xFF;
            init_code[4] = 0xB1; /* return */
            
            /* Constructor: Material() */
            create_stub_method(clazz, "<init>", "()V", ACC_PUBLIC, init_code, 5);
        }
        
        /* Special case for M3G Texture2D: needs constructor (Image2D) */
        if (strcmp(name, "javax/microedition/m3g/Texture2D") == 0) {
            uint16_t super_init_ref = add_method_ref(clazz, "javax/microedition/m3g/Object3D", "<init>", "()V");
            
            uint8_t init_code[5];
            init_code[0] = 0x2A; /* aload_0 */
            init_code[1] = 0xB7; /* invokespecial */
            init_code[2] = (super_init_ref >> 8) & 0xFF;
            init_code[3] = super_init_ref & 0xFF;
            init_code[4] = 0xB1; /* return */
            
            /* Constructor: Texture2D(Image2D image) */
            create_stub_method(clazz, "<init>", "(Ljavax/microedition/m3g/Image2D;)V", ACC_PUBLIC, init_code, 5);
        }
        
        /* Special case for M3G Image2D: needs constructor (int, int, int) */
        if (strcmp(name, "javax/microedition/m3g/Image2D") == 0) {
            uint16_t super_init_ref = add_method_ref(clazz, "javax/microedition/m3g/Object3D", "<init>", "()V");
            
            uint8_t init_code[5];
            init_code[0] = 0x2A; /* aload_0 */
            init_code[1] = 0xB7; /* invokespecial */
            init_code[2] = (super_init_ref >> 8) & 0xFF;
            init_code[3] = super_init_ref & 0xFF;
            init_code[4] = 0xB1; /* return */
            
            /* Constructor: Image2D(int format, int width, int height) */
            create_stub_method(clazz, "<init>", "(III)V", ACC_PUBLIC, init_code, 5);
            
            /* Constructor: Image2D(int format, Object image) */
            create_stub_method(clazz, "<init>", "(ILjava/lang/Object;)V", ACC_PUBLIC, init_code, 5);
        }
        
        /* Special case for M3G World: needs default constructor */
        if (strcmp(name, "javax/microedition/m3g/World") == 0) {
            uint16_t super_init_ref = add_method_ref(clazz, "javax/microedition/m3g/Group", "<init>", "()V");
            
            uint8_t init_code[5];
            init_code[0] = 0x2A; /* aload_0 */
            init_code[1] = 0xB7; /* invokespecial */
            init_code[2] = (super_init_ref >> 8) & 0xFF;
            init_code[3] = super_init_ref & 0xFF;
            init_code[4] = 0xB1; /* return */
            
            /* Constructor: World() */
            create_stub_method(clazz, "<init>", "()V", ACC_PUBLIC, init_code, 5);
        }
        
        /* Special case for M3G Sprite3D: needs constructors */
        if (strcmp(name, "javax/microedition/m3g/Sprite3D") == 0) {
            uint16_t super_init_ref = add_method_ref(clazz, "javax/microedition/m3g/Node", "<init>", "()V");
            
            uint8_t init_code[5];
            init_code[0] = 0x2A; /* aload_0 */
            init_code[1] = 0xB7; /* invokespecial */
            init_code[2] = (super_init_ref >> 8) & 0xFF;
            init_code[3] = super_init_ref & 0xFF;
            init_code[4] = 0xB1; /* return */
            
            /* Constructor: Sprite3D() - default */
            create_stub_method(clazz, "<init>", "()V", ACC_PUBLIC, init_code, 5);
        }
        
        /* Special case for Thread: needs constructors with name and runnable */
        if (strcmp(name, "java/lang/Thread") == 0) {
            uint16_t super_init_ref = add_method_ref(clazz, "java/lang/Object", "<init>", "()V");
            
            /* Field refs for Thread */
            uint16_t name_field_ref = add_field_ref(clazz, "java/lang/Thread", "name", "Ljava/lang/String;");
            uint16_t priority_field_ref = add_field_ref(clazz, "java/lang/Thread", "priority", "I");
            uint16_t target_field_ref = add_field_ref(clazz, "java/lang/Thread", "target", "Ljava/lang/Runnable;");
            
            /* Thread() - default constructor, sets priority to 5 (NORM_PRIORITY) */
            /* Code: aload_0, invokespecial Object.<init>, aload_0, iconst_5, putfield priority, return */
            uint8_t* init_default_code = malloc(10);
            init_default_code[0] = 0x2A; /* aload_0 */
            init_default_code[1] = 0xB7; /* invokespecial */
            init_default_code[2] = (super_init_ref >> 8) & 0xFF;
            init_default_code[3] = super_init_ref & 0xFF;
            init_default_code[4] = 0x2A; /* aload_0 */
            init_default_code[5] = 0x08; /* iconst_5 */
            init_default_code[6] = 0xB5; /* putfield */
            init_default_code[7] = (priority_field_ref >> 8) & 0xFF;
            init_default_code[8] = priority_field_ref & 0xFF;
            init_default_code[9] = 0xB1; /* return */
            create_stub_method(clazz, "<init>", "()V", ACC_PUBLIC, init_default_code, 10);
            
            /* Thread(String name) */
            /* Code: aload_0, invokespecial Object.<init>, aload_0, iconst_5, putfield priority, aload_0, aload_1, putfield name, return */
            uint8_t* init_string_code = malloc(16);
            init_string_code[0] = 0x2A; /* aload_0 */
            init_string_code[1] = 0xB7; /* invokespecial */
            init_string_code[2] = (super_init_ref >> 8) & 0xFF;
            init_string_code[3] = super_init_ref & 0xFF;
            init_string_code[4] = 0x2A; /* aload_0 */
            init_string_code[5] = 0x08; /* iconst_5 */
            init_string_code[6] = 0xB5; /* putfield priority */
            init_string_code[7] = (priority_field_ref >> 8) & 0xFF;
            init_string_code[8] = priority_field_ref & 0xFF;
            init_string_code[9] = 0x2A; /* aload_0 */
            init_string_code[10] = 0x2B; /* aload_1 */
            init_string_code[11] = 0xB5; /* putfield name */
            init_string_code[12] = (name_field_ref >> 8) & 0xFF;
            init_string_code[13] = name_field_ref & 0xFF;
            init_string_code[14] = 0xB1; /* return */
            init_string_code[15] = 0x00; /* padding */
            create_stub_method(clazz, "<init>", "(Ljava/lang/String;)V", ACC_PUBLIC, init_string_code, 15);
            
            /* Thread(Runnable target) */
            /* Code: aload_0, invokespecial Object.<init>, aload_0, iconst_5, putfield priority, aload_0, aload_1, putfield target, return */
            uint8_t* init_runnable_code = malloc(16);
            init_runnable_code[0] = 0x2A; /* aload_0 */
            init_runnable_code[1] = 0xB7; /* invokespecial */
            init_runnable_code[2] = (super_init_ref >> 8) & 0xFF;
            init_runnable_code[3] = super_init_ref & 0xFF;
            init_runnable_code[4] = 0x2A; /* aload_0 */
            init_runnable_code[5] = 0x08; /* iconst_5 */
            init_runnable_code[6] = 0xB5; /* putfield priority */
            init_runnable_code[7] = (priority_field_ref >> 8) & 0xFF;
            init_runnable_code[8] = priority_field_ref & 0xFF;
            init_runnable_code[9] = 0x2A; /* aload_0 */
            init_runnable_code[10] = 0x2B; /* aload_1 */
            init_runnable_code[11] = 0xB5; /* putfield target */
            init_runnable_code[12] = (target_field_ref >> 8) & 0xFF;
            init_runnable_code[13] = target_field_ref & 0xFF;
            init_runnable_code[14] = 0xB1; /* return */
            init_runnable_code[15] = 0x00; /* padding */
            create_stub_method(clazz, "<init>", "(Ljava/lang/Runnable;)V", ACC_PUBLIC, init_runnable_code, 15);
            
            /* Thread(Runnable target, String name) */
            /* Code: aload_0, invokespecial Object.<init>, aload_0, iconst_5, putfield priority, 
                     aload_0, aload_1, putfield target, aload_0, aload_2, putfield name, return */
            uint8_t* init_runnable_string_code = malloc(21);
            init_runnable_string_code[0] = 0x2A; /* aload_0 */
            init_runnable_string_code[1] = 0xB7; /* invokespecial */
            init_runnable_string_code[2] = (super_init_ref >> 8) & 0xFF;
            init_runnable_string_code[3] = super_init_ref & 0xFF;
            init_runnable_string_code[4] = 0x2A; /* aload_0 */
            init_runnable_string_code[5] = 0x08; /* iconst_5 */
            init_runnable_string_code[6] = 0xB5; /* putfield priority */
            init_runnable_string_code[7] = (priority_field_ref >> 8) & 0xFF;
            init_runnable_string_code[8] = priority_field_ref & 0xFF;
            init_runnable_string_code[9] = 0x2A; /* aload_0 */
            init_runnable_string_code[10] = 0x2B; /* aload_1 (target) */
            init_runnable_string_code[11] = 0xB5; /* putfield target */
            init_runnable_string_code[12] = (target_field_ref >> 8) & 0xFF;
            init_runnable_string_code[13] = target_field_ref & 0xFF;
            init_runnable_string_code[14] = 0x2A; /* aload_0 */
            init_runnable_string_code[15] = 0x2C; /* aload_2 (name) */
            init_runnable_string_code[16] = 0xB5; /* putfield name */
            init_runnable_string_code[17] = (name_field_ref >> 8) & 0xFF;
            init_runnable_string_code[18] = name_field_ref & 0xFF;
            init_runnable_string_code[19] = 0xB1; /* return */
            init_runnable_string_code[20] = 0x00; /* padding */
            create_stub_method(clazz, "<init>", "(Ljava/lang/Runnable;Ljava/lang/String;)V", ACC_PUBLIC, init_runnable_string_code, 20);
        }
        
        /* v34.3 FIX: the stub bytecode constructors that used to live here
         * only called super.<init>() and silently DROPPED their arguments.
         * Before v34 the execute.c stub shortcut dispatched them to the
         * registered native <init>; after the v34 narrowing ("stub shortcut
         * only for methods WITHOUT bytecode") the no-op bytecode won and the
         * native never ran (ByteArrayInputStream kept buf=NULL/count=0 ->
         * Manager.createPlayer threw MediaException; Alert lost title/text;
         * Sound/StringBuffer lost their data). All these constructors have
         * registered natives now - resolution reaches them through the
         * op_invokespecial constructor fallback (v18 FIX), so the block is
         * simply gone. See FIXES.txt v34.3. */
        
        /* v34.3 FIX: the stub bytecode constructors that used to live here
         * only called super.<init>() and silently DROPPED their arguments.
         * Before v34 the execute.c stub shortcut dispatched them to the
         * registered native <init>; after the v34 narrowing ("stub shortcut
         * only for methods WITHOUT bytecode") the no-op bytecode won and the
         * native never ran (ByteArrayInputStream kept buf=NULL/count=0 ->
         * Manager.createPlayer threw MediaException; Alert lost title/text;
         * Sound/StringBuffer lost their data). All these constructors have
         * registered natives now - resolution reaches them through the
         * op_invokespecial constructor fallback (v18 FIX), so the block is
         * simply gone. See FIXES.txt v34.3. */
        
        /* Special case for ByteArrayOutputStream: needs default constructor */
        if (strcmp(name, "java/io/ByteArrayOutputStream") == 0) {
            /* Default constructor already added, but native handler initializes buffer */
        }
        
        /* Special case for Vector: needs constructors (int), (int, int) */
        if (strcmp(name, "java/util/Vector") == 0) {
            /* Constructor: Vector(int initialCapacity) */
            uint16_t super_init_ref = add_method_ref(clazz, "java/lang/Object", "<init>", "()V");
            
            uint8_t init_int_code[5];
            init_int_code[0] = 0x2A; /* aload_0 */
            init_int_code[1] = 0xB7; /* invokespecial */
            init_int_code[2] = (super_init_ref >> 8) & 0xFF;
            init_int_code[3] = super_init_ref & 0xFF;
            init_int_code[4] = 0xB1; /* return */
            
            create_stub_method(clazz, "<init>", "(I)V", ACC_PUBLIC, init_int_code, 5);
            
            /* Constructor: Vector(int initialCapacity, int capacityIncrement) */
            uint8_t init_int_int_code[5];
            init_int_int_code[0] = 0x2A; /* aload_0 */
            init_int_int_code[1] = 0xB7; /* invokespecial */
            init_int_int_code[2] = (super_init_ref >> 8) & 0xFF;
            init_int_int_code[3] = super_init_ref & 0xFF;
            init_int_int_code[4] = 0xB1; /* return */
            
            create_stub_method(clazz, "<init>", "(II)V", ACC_PUBLIC, init_int_int_code, 5);
        }
        
        /* Special case for Hashtable: needs constructor (int), (int, float) */
        if (strcmp(name, "java/util/Hashtable") == 0) {
            uint16_t super_init_ref = add_method_ref(clazz, "java/lang/Object", "<init>", "()V");
            
            /* Constructor: Hashtable(int initialCapacity) */
            uint8_t init_int_code[5];
            init_int_code[0] = 0x2A; /* aload_0 */
            init_int_code[1] = 0xB7; /* invokespecial */
            init_int_code[2] = (super_init_ref >> 8) & 0xFF;
            init_int_code[3] = super_init_ref & 0xFF;
            init_int_code[4] = 0xB1; /* return */
            
            create_stub_method(clazz, "<init>", "(I)V", ACC_PUBLIC, init_int_code, 5);
            
            /* Constructor: Hashtable(int initialCapacity, float loadFactor) */
            uint8_t init_int_float_code[5];
            init_int_float_code[0] = 0x2A; /* aload_0 */
            init_int_float_code[1] = 0xB7; /* invokespecial */
            init_int_float_code[2] = (super_init_ref >> 8) & 0xFF;
            init_int_float_code[3] = super_init_ref & 0xFF;
            init_int_float_code[4] = 0xB1; /* return */
            
            create_stub_method(clazz, "<init>", "(IF)V", ACC_PUBLIC, init_int_float_code, 5);
        }
        
        /* v34.3 FIX: the stub bytecode constructors that used to live here
         * only called super.<init>() and silently DROPPED their arguments.
         * Before v34 the execute.c stub shortcut dispatched them to the
         * registered native <init>; after the v34 narrowing ("stub shortcut
         * only for methods WITHOUT bytecode") the no-op bytecode won and the
         * native never ran (ByteArrayInputStream kept buf=NULL/count=0 ->
         * Manager.createPlayer threw MediaException; Alert lost title/text;
         * Sound/StringBuffer lost their data). All these constructors have
         * registered natives now - resolution reaches them through the
         * op_invokespecial constructor fallback (v18 FIX), so the block is
         * simply gone. See FIXES.txt v34.3. */
        
        /* v34.3 FIX: the stub bytecode constructors that used to live here
         * only called super.<init>() and silently DROPPED their arguments.
         * Before v34 the execute.c stub shortcut dispatched them to the
         * registered native <init>; after the v34 narrowing ("stub shortcut
         * only for methods WITHOUT bytecode") the no-op bytecode won and the
         * native never ran (ByteArrayInputStream kept buf=NULL/count=0 ->
         * Manager.createPlayer threw MediaException; Alert lost title/text;
         * Sound/StringBuffer lost their data). All these constructors have
         * registered natives now - resolution reaches them through the
         * op_invokespecial constructor fallback (v18 FIX), so the block is
         * simply gone. See FIXES.txt v34.3. */
        
        /* Special case for DataInputStream: needs constructor (InputStream) */
        if (strcmp(name, "java/io/DataInputStream") == 0) {
            uint16_t super_init_ref = add_method_ref(clazz, "java/io/InputStream", "<init>", "()V");
            uint16_t in_field_ref = add_field_ref(clazz, "java/io/DataInputStream", "in", "Ljava/io/InputStream;");

            /* v34.2 FIX (Stalker: static picture / EOFException while parsing
             * the '/d' resource): this stub constructor used to be ONLY
             *   aload_0; invokespecial InputStream.<init>; return
             * and relied on native_datainputstream_init (native registry)
             * setting the "in" field. The v34 shortcut restriction
             * ("stub shortcut only for methods WITHOUT bytecode") made the
             * real bytecode run instead - the native <init> was never
             * dispatched, "in" stayed NULL and every DataInputStream read
             * returned EOF from the first byte. Games parse their resources
             * in <clinit> via readShort() loops -> parse aborts -> static
             * arrays stay NULL -> paint() NPEs every frame -> frozen image.
             * Store the stream in bytecode, exactly like the
             * InputStreamReader/BufferedReader stubs above:
             *   aload_0; aload_1; putfield in; aload_0;
             *   invokespecial InputStream.<init>; return */
            uint8_t init_code[10];
            init_code[0] = 0x2A; /* aload_0 */
            init_code[1] = 0x2B; /* aload_1 (InputStream) */
            init_code[2] = 0xB5; /* putfield */
            init_code[3] = (in_field_ref >> 8) & 0xFF;
            init_code[4] = in_field_ref & 0xFF;
            init_code[5] = 0x2A; /* aload_0 */
            init_code[6] = 0xB7; /* invokespecial */
            init_code[7] = (super_init_ref >> 8) & 0xFF;
            init_code[8] = super_init_ref & 0xFF;
            init_code[9] = 0xB1; /* return */

            create_stub_method(clazz, "<init>", "(Ljava/io/InputStream;)V", ACC_PUBLIC, init_code, 10);
        }
        
        /* Special case for InputStreamReader: needs constructor (InputStream) */
        if (strcmp(name, "java/io/InputStreamReader") == 0) {
            uint16_t super_init_ref = add_method_ref(clazz, "java/io/Reader", "<init>", "()V");
            uint16_t in_field_ref = add_field_ref(clazz, "java/io/InputStreamReader", "in", "Ljava/io/InputStream;");
            
            /* Constructor: InputStreamReader(InputStream)
             * Code: aload_0, aload_1, putfield in, aload_0, invokespecial Reader.<init>, return
             */
            uint8_t init_code[10];
            init_code[0] = 0x2A; /* aload_0 */
            init_code[1] = 0x2B; /* aload_1 (InputStream) */
            init_code[2] = 0xB5; /* putfield */
            init_code[3] = (in_field_ref >> 8) & 0xFF;
            init_code[4] = in_field_ref & 0xFF;
            init_code[5] = 0x2A; /* aload_0 */
            init_code[6] = 0xB7; /* invokespecial */
            init_code[7] = (super_init_ref >> 8) & 0xFF;
            init_code[8] = super_init_ref & 0xFF;
            init_code[9] = 0xB1; /* return */
            
            create_stub_method(clazz, "<init>", "(Ljava/io/InputStream;)V", ACC_PUBLIC, init_code, 10);
            
            /* Constructor with encoding: InputStreamReader(InputStream, String) - same logic, ignore String */
            create_stub_method(clazz, "<init>", "(Ljava/io/InputStream;Ljava/lang/String;)V", ACC_PUBLIC, init_code, 10);
        }
        
        /* Special case for BufferedReader: needs constructor (Reader) */
        if (strcmp(name, "java/io/BufferedReader") == 0) {
            uint16_t super_init_ref = add_method_ref(clazz, "java/io/Reader", "<init>", "()V");
            uint16_t in_field_ref = add_field_ref(clazz, "java/io/BufferedReader", "in", "Ljava/io/Reader;");
            
            /* Constructor: BufferedReader(Reader)
             * Code: aload_0, aload_1, putfield in, aload_0, invokespecial Reader.<init>, return
             */
            uint8_t init_code[10];
            init_code[0] = 0x2A; /* aload_0 */
            init_code[1] = 0x2B; /* aload_1 (Reader) */
            init_code[2] = 0xB5; /* putfield */
            init_code[3] = (in_field_ref >> 8) & 0xFF;
            init_code[4] = in_field_ref & 0xFF;
            init_code[5] = 0x2A; /* aload_0 */
            init_code[6] = 0xB7; /* invokespecial */
            init_code[7] = (super_init_ref >> 8) & 0xFF;
            init_code[8] = super_init_ref & 0xFF;
            init_code[9] = 0xB1; /* return */
            
            create_stub_method(clazz, "<init>", "(Ljava/io/Reader;)V", ACC_PUBLIC, init_code, 10);
            create_stub_method(clazz, "<init>", "(Ljava/io/Reader;I)V", ACC_PUBLIC, init_code, 10);
        }
        
        /* Special case for Throwable: needs constructors (String), (Throwable), (String, Throwable) */
        if (strcmp(name, "java/lang/Throwable") == 0) {
            /* Constructor Throwable(String) - stores message in detailMessage field */
            uint16_t super_init_ref = add_method_ref(clazz, "java/lang/Object", "<init>", "()V");
            uint16_t detail_msg_field_ref = add_field_ref(clazz, "java/lang/Throwable", "detailMessage", "Ljava/lang/String;");
            
            /* Code: aload_0, invokespecial Object.<init>, aload_0, aload_1, putfield detailMessage, */
            uint8_t init_str_code[10];
            init_str_code[0] = 0x2A; /* aload_0 */
            init_str_code[1] = 0xB7; /* invokespecial */
            init_str_code[2] = (super_init_ref >> 8) & 0xFF;
            init_str_code[3] = super_init_ref & 0xFF;
            init_str_code[4] = 0x2A; /* aload_0 */
            init_str_code[5] = 0x2B; /* aload_1 (String message) */
            init_str_code[6] = 0xB5; /* putfield */
            init_str_code[7] = (detail_msg_field_ref >> 8) & 0xFF;
            init_str_code[8] = detail_msg_field_ref & 0xFF;
            init_str_code[9] = 0xB1; /* return */
            
            create_stub_method(clazz, "<init>", "(Ljava/lang/String;)V", ACC_PUBLIC, init_str_code, 10);
            
            /* Constructor Throwable(Throwable) - stores cause in detailMessage via toString */
            /* For simplicity, just calls super() - cause handling would be more complex */
            uint8_t init_throw_code[5];
            init_throw_code[0] = 0x2A; /* aload_0 */
            init_throw_code[1] = 0xB7; /* invokespecial */
            init_throw_code[2] = (super_init_ref >> 8) & 0xFF;
            init_throw_code[3] = super_init_ref & 0xFF;
            init_throw_code[4] = 0xB1; /* return */
            create_stub_method(clazz, "<init>", "(Ljava/lang/Throwable;)V", ACC_PUBLIC, init_throw_code, 5);
            
            /* Constructor Throwable(String, Throwable) - stores message, ignores cause */
            uint8_t init_str_throw_code[10];
            init_str_throw_code[0] = 0x2A; /* aload_0 */
            init_str_throw_code[1] = 0xB7; /* invokespecial */
            init_str_throw_code[2] = (super_init_ref >> 8) & 0xFF;
            init_str_throw_code[3] = super_init_ref & 0xFF;
            init_str_throw_code[4] = 0x2A; /* aload_0 */
            init_str_throw_code[5] = 0x2B; /* aload_1 (String message) */
            init_str_throw_code[6] = 0xB5; /* putfield */
            init_str_throw_code[7] = (detail_msg_field_ref >> 8) & 0xFF;
            init_str_throw_code[8] = detail_msg_field_ref & 0xFF;
            init_str_throw_code[9] = 0xB1; /* return */
            create_stub_method(clazz, "<init>", "(Ljava/lang/String;Ljava/lang/Throwable;)V", ACC_PUBLIC, init_str_throw_code, 10);
            
            /* getMessage() method - returns detailMessage field */
            uint8_t get_msg_code[6];
            get_msg_code[0] = 0x2A; /* aload_0 */
            get_msg_code[1] = 0xB4; /* getfield */
            get_msg_code[2] = (detail_msg_field_ref >> 8) & 0xFF;
            get_msg_code[3] = detail_msg_field_ref & 0xFF;
            get_msg_code[4] = 0xB0; /* areturn */
            /* Need a unique method ref for getMessage, so we use putfield ref which won't work */
            /* Actually, getfield uses field_ref, which we already have */
            create_stub_method(clazz, "getMessage", "()Ljava/lang/String;", ACC_PUBLIC, get_msg_code, 5);
            
            /* toString() method for Throwable - uses native implementation */
            /* We mark it as native and handle it in native.c */
            /* The code is complex (getClass().getName() + getMessage()) so use native */
            create_stub_method(clazz, "toString", "()Ljava/lang/String;", ACC_PUBLIC | ACC_NATIVE, NULL, 0);
            
            /* getLocalizedMessage() method - returns getMessage() by default */
            /* Same code as getMessage - just call getMessage() */
            uint8_t get_local_msg_code[5];
            get_local_msg_code[0] = 0x2A; /* aload_0 */
            get_local_msg_code[1] = 0xB4; /* getfield */
            get_local_msg_code[2] = (detail_msg_field_ref >> 8) & 0xFF;
            get_local_msg_code[3] = detail_msg_field_ref & 0xFF;
            get_local_msg_code[4] = 0xB0; /* areturn */
            create_stub_method(clazz, "getLocalizedMessage", "()Ljava/lang/String;", ACC_PUBLIC, get_local_msg_code, 5);
            
            /* getStackTrace() method - returns stackTrace field */
            uint16_t stack_trace_field_ref = add_field_ref(clazz, "java/lang/Throwable", "stackTrace", "[Ljava/lang/StackTraceElement;");
            uint8_t get_stack_trace_code[5];
            get_stack_trace_code[0] = 0x2A; /* aload_0 */
            get_stack_trace_code[1] = 0xB4; /* getfield */
            get_stack_trace_code[2] = (stack_trace_field_ref >> 8) & 0xFF;
            get_stack_trace_code[3] = stack_trace_field_ref & 0xFF;
            get_stack_trace_code[4] = 0xB0; /* areturn */
            create_stub_method(clazz, "getStackTrace", "()[Ljava/lang/StackTraceElement;", ACC_PUBLIC, get_stack_trace_code, 5);
            
            /* fillInStackTrace() method - native implementation */
            create_stub_method(clazz, "fillInStackTrace", "()Ljava/lang/Throwable;", ACC_PUBLIC | ACC_NATIVE, NULL, 0);
            
            /* printStackTrace() method - native implementation */
            create_stub_method(clazz, "printStackTrace", "()V", ACC_PUBLIC | ACC_NATIVE, NULL, 0);
            
            /* Default constructor ()V - just calls super */
            uint8_t init_default_code[5];
            init_default_code[0] = 0x2A; /* aload_0 */
            init_default_code[1] = 0xB7; /* invokespecial */
            init_default_code[2] = (super_init_ref >> 8) & 0xFF;
            init_default_code[3] = super_init_ref & 0xFF;
            init_default_code[4] = 0xB1; /* return */
            create_stub_method(clazz, "<init>", "()V", ACC_PUBLIC, init_default_code, 5);
        }
        
        /* Для классов исключений добавляем конструктор с сообщением */
        if ((strstr(name, "Exception") || strstr(name, "Error")) && strcmp(name, "java/lang/Throwable") != 0) {
            /* Конструктор с сообщением: super(msg) */
            uint16_t super_init_msg_ref = add_method_ref(clazz, super_name, "<init>", "(Ljava/lang/String;)V");
            
            uint8_t init_msg_code[6];
            init_msg_code[0] = 0x2A; /* aload_0 */
            init_msg_code[1] = 0x2B; /* aload_1 (загружаем сообщение) */
            init_msg_code[2] = 0xB7; /* invokespecial */
            init_msg_code[3] = (super_init_msg_ref >> 8) & 0xFF;
            init_msg_code[4] = super_init_msg_ref & 0xFF;
            init_msg_code[5] = 0xB1; /* return */
            
            create_stub_method(clazz, "<init>", "(Ljava/lang/String;)V", ACC_PUBLIC, init_msg_code, 6);
            
            /* Конструктор с сообщением и причиной (Throwable) */
            uint16_t super_init_msg_throw_ref = add_method_ref(clazz, super_name, "<init>", "(Ljava/lang/String;Ljava/lang/Throwable;)V");
            
            uint8_t init_msg_throw_code[8];
            init_msg_throw_code[0] = 0x2A; /* aload_0 */
            init_msg_throw_code[1] = 0x2B; /* aload_1 (сообщение) */
            init_msg_throw_code[2] = 0x2C; /* aload_2 (причина) */
            init_msg_throw_code[3] = 0xB7; /* invokespecial */
            init_msg_throw_code[4] = (super_init_msg_throw_ref >> 8) & 0xFF;
            init_msg_throw_code[5] = super_init_msg_throw_ref & 0xFF;
            init_msg_throw_code[6] = 0xB1; /* return */
            
            create_stub_method(clazz, "<init>", "(Ljava/lang/String;Ljava/lang/Throwable;)V", 
                              ACC_PUBLIC, init_msg_throw_code, 7);
        }
    }
    
    /* Mark as stub */
    clazz->is_stub = true;
    clazz->initialized = false;
    clazz->initializing = false;
    /* FIX-19t: do NOT reset static_fields here! clazz comes from calloc()
     * (already NULL) and the special-case blocks above (Canvas constants,
     * System fields, ...) may have already populated static storage -
     * resetting it leaked that memory and silently dropped Canvas.UP/DOWN
     * etc., collapsing all game actions to key code 0. */

    /* CRITICAL FIX (v15): default field capacity for stub objects.
     *
     * Native implementations routinely store per-object state in the raw
     * fields[] slots of stub-class instances (RecordEnumerationImpl keeps
     * {ids array ref, currentIndex, store handle}, M3G/LCDUI stubs keep
     * nativePeer-ish handles, etc.). Historically a stub class without
     * explicitly declared fields got instance_size == sizeof(ObjectHeader),
     * so the FIRST instance allocated for such a class had NO room for
     * fields: native writes then landed on the GC header of the NEXT heap
     * object - silent heap corruption that desyncs the GC linear sweep
     * (crashes / wedged GC on Windows, "corrupted magic" floods on Linux).
     *
     * setup_stub_class_fields() overrides instance_size for every class it
     * declares fields for, so this default only affects fieldless stubs -
     * exactly the dangerous ones. 8 slots covers all current native writers
     * (grep '->fields\[' in midp/ shows max 7 used on fieldless stubs). */
    if (clazz->instance_size < sizeof(ObjectHeader) + 8 * sizeof(JavaValue)) {
        clazz->instance_size = sizeof(ObjectHeader) + 8 * sizeof(JavaValue);
    }

    return clazz;
}

/* v34.15: interface wiring for concrete media stub classes (see forward
 * declaration comment above). Idempotent: only fills interfaces if the
 * stub has none yet, so calling from both creation paths is safe. */
static void stub_add_known_interfaces(JavaClass* stub) {
    if (!stub || !stub->class_name) return;
    if (stub->interfaces_count > 0) return;  /* already wired */

    if (strcmp(stub->class_name, "javax/microedition/media/PlayerImpl") == 0) {
        /* PlayerImpl implements Player and Controllable interfaces */
        stub->interfaces_count = 2;
        stub->interfaces = (uint16_t*)calloc(2, sizeof(uint16_t));
        if (stub->interfaces) {
            stub->interfaces[0] = add_class_ref(stub, "javax/microedition/media/Player");
            stub->interfaces[1] = add_class_ref(stub, "javax/microedition/media/Controllable");
            CLASS_DEBUG("Added interfaces to PlayerImpl: Player, Controllable");
        }
    } else if (strcmp(stub->class_name, "javax/microedition/media/control/VolumeControlImpl") == 0) {
        /* VolumeControlImpl implements VolumeControl */
        stub->interfaces_count = 1;
        stub->interfaces = (uint16_t*)calloc(1, sizeof(uint16_t));
        if (stub->interfaces) {
            stub->interfaces[0] = add_class_ref(stub, "javax/microedition/media/control/VolumeControl");
            CLASS_DEBUG("Added interface to VolumeControlImpl: VolumeControl");
        }
    } else if (strcmp(stub->class_name, "javax/microedition/media/control/MIDIControlImpl") == 0) {
        stub->interfaces_count = 1;
        stub->interfaces = (uint16_t*)calloc(1, sizeof(uint16_t));
        if (stub->interfaces) {
            stub->interfaces[0] = add_class_ref(stub, "javax/microedition/media/control/MIDIControl");
        }
    } else if (strcmp(stub->class_name, "javax/microedition/media/control/ToneControlImpl") == 0) {
        stub->interfaces_count = 1;
        stub->interfaces = (uint16_t*)calloc(1, sizeof(uint16_t));
        if (stub->interfaces) {
            stub->interfaces[0] = add_class_ref(stub, "javax/microedition/media/control/ToneControl");
        }
    } else if (strcmp(stub->class_name, "javax/microedition/media/control/VideoControlImpl") == 0) {
        stub->interfaces_count = 2;
        stub->interfaces = (uint16_t*)calloc(2, sizeof(uint16_t));
        if (stub->interfaces) {
            stub->interfaces[0] = add_class_ref(stub, "javax/microedition/media/control/VideoControl");
            stub->interfaces[1] = add_class_ref(stub, "javax/microedition/media/control/GUIControl");
        }
    }
}

/* Initialize built-in stub classes */
int init_stub_classes(JVM* jvm) {
    if (!jvm) return 0;
    
    int count = 0;
    
    /* ===== ПЕРВЫЙ ПРОХОД: Создаем все классы ===== */
    for (int i = 0; stub_classes[i].name != NULL; i++) {
        /* Check if already loaded */
        bool found = false;
        for (size_t j = 0; j < jvm->class_loader.count; j++) {
            if (jvm->class_loader.classes[j]->class_name &&
                strcmp(jvm->class_loader.classes[j]->class_name, stub_classes[i].name) == 0) {
                found = true;
                break;
            }
        }
        
        if (found) continue;
        
        JavaClass* stub = create_stub_class(jvm, stub_classes[i].name,
                                           stub_classes[i].super, stub_classes[i].flags);
        if (!stub) {
            CLASS_DEBUG(" Failed to create stub class: %s", stub_classes[i].name);
            continue;
        }
        
        /* Ensure class_loader has capacity */
        if (jvm->class_loader.count >= jvm->class_loader.capacity) {
            size_t new_cap = jvm->class_loader.capacity ? jvm->class_loader.capacity * 2 : 32;
            jvm->class_loader.classes = (JavaClass**)realloc(jvm->class_loader.classes,
                                   new_cap * sizeof(JavaClass*));
            if (jvm->class_loader.classes) {
                jvm->class_loader.capacity = new_cap;
            } else {
                free(stub);
                continue;
            }
        }
        
        jvm->class_loader.classes[jvm->class_loader.count++] = stub;
        /* v34.15: wire "XyzImpl implements Xyz" links here too — the boot
         * path bypassed get_or_create_stub_class() and left media control
         * stubs without interfaces (root cause of the GoF VolumeControl
         * ClassCastException). */
        stub_add_known_interfaces(stub);
        count++;
        
        if (jvm->config.verbose_class) {
            CLASS_DEBUG(" Created stub class: %s", stub_classes[i].name);
        }
    }
    
    /* ===== ВТОРОЙ ПРОХОД: Разрешаем ссылки на суперклассы ===== */
    for (size_t i = 0; i < jvm->class_loader.count; i++) {
        JavaClass* clazz = jvm->class_loader.classes[i];
        if (clazz->super_class == NULL && clazz->super_class_name != NULL) {
            /* Find super class in loaded classes */
            for (size_t j = 0; j < jvm->class_loader.count; j++) {
                if (jvm->class_loader.classes[j]->class_name &&
                    strcmp(jvm->class_loader.classes[j]->class_name, clazz->super_class_name) == 0) {
                    clazz->super_class = jvm->class_loader.classes[j];
                    if (jvm->config.verbose_class) {
                        CLASS_DEBUG(" Resolved %s -> super: %s", 
                                clazz->class_name, clazz->super_class->class_name);
                    }
                    break;
                }
            }
        }
    }
    
    /* ===== ТРЕТИЙ ПРОХОД: Добавляем поля к классам ===== */
    for (size_t i = 0; i < jvm->class_loader.count; i++) {
        JavaClass* clazz = jvm->class_loader.classes[i];
        if (!clazz->class_name) continue;
        
        /* java.lang.System - статические поля */
        if (strcmp(clazz->class_name, "java/lang/System") == 0) {
            /* Создаем массив статических полей */
            static const char* sys_static_fields[][2] = {
                {"out", "Ljava/io/PrintStream;"},
                {"err", "Ljava/io/PrintStream;"},
                {"in", "Ljava/io/InputStream;"},
                {NULL, NULL}
            };
            
            int static_count = 0;
            while (sys_static_fields[static_count][0] != NULL) static_count++;
            
            clazz->static_fields = (JavaStaticField*)calloc(static_count, sizeof(JavaStaticField));
            clazz->static_fields_count = static_count;
            clazz->static_fields_capacity = static_count;
            
            for (int j = 0; j < static_count; j++) {
                clazz->static_fields[j].name = strdup(sys_static_fields[j][0]);
                clazz->static_fields[j].descriptor = strdup(sys_static_fields[j][1]);
                memset(&clazz->static_fields[j].value, 0, sizeof(JavaValue));
            }
            
            CLASS_DEBUG(" System class: added %d static fields", static_count);
        }
        
        /* java.lang.Throwable needs detailMessage and stackTrace fields */
        else if (clazz->class_name && strcmp(clazz->class_name, "java/lang/Throwable") == 0 && clazz->fields_count == 0) {
            clazz->fields_count = 2;
            clazz->fields = (JavaField*)calloc(2, sizeof(JavaField));
            if (clazz->fields) {
                /* detailMessage field */
                clazz->fields[0].name = strdup("detailMessage");
                clazz->fields[0].descriptor = strdup("Ljava/lang/String;");
                clazz->fields[0].name_index = add_utf8(clazz, "detailMessage");
                clazz->fields[0].descriptor_index = add_utf8(clazz, "Ljava/lang/String;");
                clazz->fields[0].access_flags = ACC_PRIVATE;
                /* stackTrace field */
                clazz->fields[1].name = strdup("stackTrace");
                clazz->fields[1].descriptor = strdup("[Ljava/lang/StackTraceElement;");
                clazz->fields[1].name_index = add_utf8(clazz, "stackTrace");
                clazz->fields[1].descriptor_index = add_utf8(clazz, "[Ljava/lang/StackTraceElement;");
                clazz->fields[1].access_flags = ACC_PRIVATE;
                clazz->instance_size = sizeof(ObjectHeader) + 2 * sizeof(JavaValue);
            }
            CLASS_DEBUG(" Added detailMessage and stackTrace fields to java/lang/Throwable\n");
        }
        
        /* java.lang.StackTraceElement needs declaringClass, methodName, fileName, lineNumber fields */
        else if (clazz->class_name && strcmp(clazz->class_name, "java/lang/StackTraceElement") == 0 && clazz->fields_count == 0) {
            clazz->fields_count = 4;
            clazz->fields = (JavaField*)calloc(4, sizeof(JavaField));
            if (clazz->fields) {
                /* declaringClass field */
                clazz->fields[0].name = strdup("declaringClass");
                clazz->fields[0].descriptor = strdup("Ljava/lang/String;");
                clazz->fields[0].name_index = add_utf8(clazz, "declaringClass");
                clazz->fields[0].descriptor_index = add_utf8(clazz, "Ljava/lang/String;");
                clazz->fields[0].access_flags = ACC_PRIVATE;
                /* methodName field */
                clazz->fields[1].name = strdup("methodName");
                clazz->fields[1].descriptor = strdup("Ljava/lang/String;");
                clazz->fields[1].name_index = add_utf8(clazz, "methodName");
                clazz->fields[1].descriptor_index = add_utf8(clazz, "Ljava/lang/String;");
                clazz->fields[1].access_flags = ACC_PRIVATE;
                /* fileName field */
                clazz->fields[2].name = strdup("fileName");
                clazz->fields[2].descriptor = strdup("Ljava/lang/String;");
                clazz->fields[2].name_index = add_utf8(clazz, "fileName");
                clazz->fields[2].descriptor_index = add_utf8(clazz, "Ljava/lang/String;");
                clazz->fields[2].access_flags = ACC_PRIVATE;
                /* lineNumber field */
                clazz->fields[3].name = strdup("lineNumber");
                clazz->fields[3].descriptor = strdup("I");
                clazz->fields[3].name_index = add_utf8(clazz, "lineNumber");
                clazz->fields[3].descriptor_index = add_utf8(clazz, "I");
                clazz->fields[3].access_flags = ACC_PRIVATE;
                clazz->instance_size = sizeof(ObjectHeader) + 4 * sizeof(JavaValue);
            }
            CLASS_DEBUG(" Added fields to java/lang/StackTraceElement\n");
        }
        
        /* java.lang.Integer needs value field - always ensure it exists */
        else if (clazz->class_name && strcmp(clazz->class_name, "java/lang/Integer") == 0) {
            /* Check if "value" field already exists */
            bool has_value_field = false;
            for (int i = 0; i < clazz->fields_count; i++) {
                if (clazz->fields && clazz->fields[i].name && 
                    strcmp(clazz->fields[i].name, "value") == 0) {
                    has_value_field = true;
                    break;
                }
            }
            
            if (!has_value_field) {
                /* Add the value field */
                int new_count = clazz->fields_count + 1;
                JavaField* new_fields = (JavaField*)realloc(clazz->fields, new_count * sizeof(JavaField));
                if (new_fields) {
                    clazz->fields = new_fields;
                    memset(&clazz->fields[clazz->fields_count], 0, sizeof(JavaField));
                    clazz->fields[clazz->fields_count].name = strdup("value");
                    clazz->fields[clazz->fields_count].descriptor = strdup("I");
                    clazz->fields[clazz->fields_count].name_index = add_utf8(clazz, "value");
                    clazz->fields[clazz->fields_count].descriptor_index = add_utf8(clazz, "I");
                    clazz->fields[clazz->fields_count].access_flags = ACC_PRIVATE;
                    clazz->fields_count = new_count;
                    clazz->instance_size = sizeof(ObjectHeader) + new_count * sizeof(JavaValue);
                    { extern uint16_t g_vm_cache_gen; g_vm_cache_gen++; }  /* v34.35 */
                    CLASS_DEBUG(" Added value field to java/lang/Integer (fields_count was %d)\n", new_count - 1);
                }
            }
        }
        
        /* java.lang.String needs value, offset, count, hash fields */
        else if (clazz->class_name && strcmp(clazz->class_name, "java/lang/String") == 0 && clazz->fields_count == 0) {
            clazz->fields_count = 4;
            clazz->fields = (JavaField*)calloc(4, sizeof(JavaField));
            if (clazz->fields) {
                /* Field 0: value - char[] array reference */
                clazz->fields[0].name = strdup("value");
                clazz->fields[0].descriptor = strdup("[C");  /* char[] */
                clazz->fields[0].name_index = add_utf8(clazz, "value");
                clazz->fields[0].descriptor_index = add_utf8(clazz, "[C");
                clazz->fields[0].access_flags = ACC_PRIVATE;
                
                /* Field 1: offset - int */
                clazz->fields[1].name = strdup("offset");
                clazz->fields[1].descriptor = strdup("I");
                clazz->fields[1].name_index = add_utf8(clazz, "offset");
                clazz->fields[1].descriptor_index = add_utf8(clazz, "I");
                clazz->fields[1].access_flags = ACC_PRIVATE;
                
                /* Field 2: count - int (string length) */
                clazz->fields[2].name = strdup("count");
                clazz->fields[2].descriptor = strdup("I");
                clazz->fields[2].name_index = add_utf8(clazz, "count");
                clazz->fields[2].descriptor_index = add_utf8(clazz, "I");
                clazz->fields[2].access_flags = ACC_PRIVATE;
                
                /* Field 3: hash - int (cached hash code) */
                clazz->fields[3].name = strdup("hash");
                clazz->fields[3].descriptor = strdup("I");
                clazz->fields[3].name_index = add_utf8(clazz, "hash");
                clazz->fields[3].descriptor_index = add_utf8(clazz, "I");
                clazz->fields[3].access_flags = ACC_PRIVATE;
                
                /* instance_size = ObjectHeader + 4 fields */
                clazz->instance_size = sizeof(ObjectHeader) + sizeof(JavaValue) * 4;
            }
            CLASS_DEBUG(" Added value/offset/count/hash fields to java/lang/String\n");
        }

        /* v38 FIX (BlackShark3D .hm parser): java.lang.Float needs a "value"
         * instance field. The game parses ALL float config via
         * `new Float(Float.parseFloat(s)).floatValue()` — with no field
         * (and no <init> storing it) every floatValue() returned 0.0f:
         * hSegmentSize/vSegmentSize became 0, af.a(FF)F divided by zero,
         * the terrain scroll loop in ai.a(FF) never terminated and the
         * mission loading froze at 95% forever. */
        else if (clazz->class_name && strcmp(clazz->class_name, "java/lang/Float") == 0) {
            bool has_value_field = false;
            for (int i = 0; i < clazz->fields_count; i++) {
                if (clazz->fields && clazz->fields[i].name &&
                    strcmp(clazz->fields[i].name, "value") == 0) {
                    has_value_field = true;
                    break;
                }
            }
            if (!has_value_field) {
                int new_count = clazz->fields_count + 1;
                JavaField* new_fields = (JavaField*)realloc(clazz->fields, new_count * sizeof(JavaField));
                if (new_fields) {
                    clazz->fields = new_fields;
                    memset(&clazz->fields[clazz->fields_count], 0, sizeof(JavaField));
                    clazz->fields[clazz->fields_count].name = strdup("value");
                    clazz->fields[clazz->fields_count].descriptor = strdup("F");
                    clazz->fields[clazz->fields_count].name_index = add_utf8(clazz, "value");
                    clazz->fields[clazz->fields_count].descriptor_index = add_utf8(clazz, "F");
                    clazz->fields[clazz->fields_count].access_flags = ACC_PRIVATE;
                    clazz->fields_count = new_count;
                    clazz->instance_size = sizeof(ObjectHeader) + new_count * sizeof(JavaValue);
                    { extern uint16_t g_vm_cache_gen; g_vm_cache_gen++; }  /* v34.35 */
                    CLASS_DEBUG(" Added value field to java/lang/Float\n");
                }
            }
        }

        /* v38 FIX: java.lang.Double needs a "value" instance field (same
         * pattern as Float — new Double(d).doubleValue() returned 0.0). */
        else if (clazz->class_name && strcmp(clazz->class_name, "java/lang/Double") == 0) {
            bool has_value_field = false;
            for (int i = 0; i < clazz->fields_count; i++) {
                if (clazz->fields && clazz->fields[i].name &&
                    strcmp(clazz->fields[i].name, "value") == 0) {
                    has_value_field = true;
                    break;
                }
            }
            if (!has_value_field) {
                int new_count = clazz->fields_count + 1;
                JavaField* new_fields = (JavaField*)realloc(clazz->fields, new_count * sizeof(JavaField));
                if (new_fields) {
                    clazz->fields = new_fields;
                    memset(&clazz->fields[clazz->fields_count], 0, sizeof(JavaField));
                    clazz->fields[clazz->fields_count].name = strdup("value");
                    clazz->fields[clazz->fields_count].descriptor = strdup("D");
                    clazz->fields[clazz->fields_count].name_index = add_utf8(clazz, "value");
                    clazz->fields[clazz->fields_count].descriptor_index = add_utf8(clazz, "D");
                    clazz->fields[clazz->fields_count].access_flags = ACC_PRIVATE;
                    clazz->fields_count = new_count;
                    /* D takes 2 JavaValue slots (header + value*2) */
                    clazz->instance_size = sizeof(ObjectHeader) + new_count * sizeof(JavaValue) + sizeof(JavaValue);
                    { extern uint16_t g_vm_cache_gen; g_vm_cache_gen++; }  /* v34.35 */
                    CLASS_DEBUG(" Added value field to java/lang/Double\n");
                }
            }
        }

        /* java.lang.Long needs value field - always ensure it exists */
        else if (clazz->class_name && strcmp(clazz->class_name, "java/lang/Long") == 0) {
            /* Check if "value" field already exists */
            bool has_value_field = false;
            for (int i = 0; i < clazz->fields_count; i++) {
                if (clazz->fields && clazz->fields[i].name && 
                    strcmp(clazz->fields[i].name, "value") == 0) {
                    has_value_field = true;
                    break;
                }
            }
            
            if (!has_value_field) {
                int new_count = clazz->fields_count + 1;
                JavaField* new_fields = (JavaField*)realloc(clazz->fields, new_count * sizeof(JavaField));
                if (new_fields) {
                    clazz->fields = new_fields;
                    memset(&clazz->fields[clazz->fields_count], 0, sizeof(JavaField));
                    clazz->fields[clazz->fields_count].name = strdup("value");
                    clazz->fields[clazz->fields_count].descriptor = strdup("J");  /* long */
                    clazz->fields[clazz->fields_count].name_index = add_utf8(clazz, "value");
                    clazz->fields[clazz->fields_count].descriptor_index = add_utf8(clazz, "J");
                    clazz->fields[clazz->fields_count].access_flags = ACC_PRIVATE;
                    clazz->fields_count = new_count;
                    clazz->instance_size = sizeof(ObjectHeader) + new_count * sizeof(JavaValue) * 2;  /* Two slots for long */
                    CLASS_DEBUG(" Added value field to java/lang/Long\n");
                }
            }
        }
        
        /* java.lang.Boolean needs value field - always ensure it exists */
        else if (clazz->class_name && strcmp(clazz->class_name, "java/lang/Boolean") == 0) {
            /* Check if "value" field already exists */
            bool has_value_field = false;
            for (int i = 0; i < clazz->fields_count; i++) {
                if (clazz->fields && clazz->fields[i].name && 
                    strcmp(clazz->fields[i].name, "value") == 0) {
                    has_value_field = true;
                    break;
                }
            }
            
            if (!has_value_field) {
                int new_count = clazz->fields_count + 1;
                JavaField* new_fields = (JavaField*)realloc(clazz->fields, new_count * sizeof(JavaField));
                if (new_fields) {
                    clazz->fields = new_fields;
                    memset(&clazz->fields[clazz->fields_count], 0, sizeof(JavaField));
                    clazz->fields[clazz->fields_count].name = strdup("value");
                    clazz->fields[clazz->fields_count].descriptor = strdup("Z");  /* boolean */
                    clazz->fields[clazz->fields_count].name_index = add_utf8(clazz, "value");
                    clazz->fields[clazz->fields_count].descriptor_index = add_utf8(clazz, "Z");
                    clazz->fields[clazz->fields_count].access_flags = ACC_PRIVATE;
                    clazz->fields_count = new_count;
                    clazz->instance_size = sizeof(ObjectHeader) + new_count * sizeof(JavaValue);
                    CLASS_DEBUG(" Added value field to java/lang/Boolean\n");
                }
            }
        }
        
        /* java.util.Random needs a seed field (long = 64-bit = 2 slots) */
        else if (clazz->class_name && strcmp(clazz->class_name, "java/util/Random") == 0 && clazz->fields_count == 0) {
            /* Allocate fields array */
            clazz->fields_count = 1;  /* One field: seed (long) */
            clazz->fields = (JavaField*)calloc(1, sizeof(JavaField));
            if (clazz->fields) {
                clazz->fields[0].name = strdup("seed");
                clazz->fields[0].descriptor = strdup("J");  /* long */
                clazz->fields[0].name_index = add_utf8(clazz, "seed");
                clazz->fields[0].descriptor_index = add_utf8(clazz, "J");
                clazz->fields[0].access_flags = ACC_PRIVATE;
                clazz->instance_size = sizeof(ObjectHeader) + sizeof(JavaValue) * 2;  /* Two slots for long */
            }
            CLASS_DEBUG(" Added seed field to java/util/Random\n");
        }
        
        /* java.lang.StringBuffer needs buffer and length fields */
        else if (clazz->class_name && strcmp(clazz->class_name, "java/lang/StringBuffer") == 0 && clazz->fields_count == 0) {
            /* Allocate fields array: [0] = buffer pointer, [1] = length */
            clazz->fields_count = 2;
            clazz->fields = (JavaField*)calloc(2, sizeof(JavaField));
            if (clazz->fields) {
                /* Field 0: buffer pointer (stored as reference) */
                clazz->fields[0].name = strdup("buffer");
                clazz->fields[0].descriptor = strdup("Ljava/lang/Object;");
                clazz->fields[0].name_index = add_utf8(clazz, "buffer");
                clazz->fields[0].descriptor_index = add_utf8(clazz, "Ljava/lang/Object;");
                clazz->fields[0].access_flags = ACC_PRIVATE;
                
                /* Field 1: length (int) */
                clazz->fields[1].name = strdup("count");
                clazz->fields[1].descriptor = strdup("I");  /* int */
                clazz->fields[1].name_index = add_utf8(clazz, "count");
                clazz->fields[1].descriptor_index = add_utf8(clazz, "I");
                clazz->fields[1].access_flags = ACC_PRIVATE;
                
                clazz->instance_size = sizeof(ObjectHeader) + sizeof(JavaValue) * 2;  /* Two JavaValue slots */
            }
            CLASS_DEBUG(" Added buffer/count fields to java/lang/StringBuffer\n");
        }
        
        /* javax.microedition.lcdui.Image needs native pointer field */
        else if (clazz->class_name && strcmp(clazz->class_name, "javax/microedition/lcdui/Image") == 0 && clazz->fields_count == 0) {
            clazz->fields_count = 1;
            clazz->fields = (JavaField*)calloc(1, sizeof(JavaField));
            if (clazz->fields) {
                /* Field 0: native pointer (stored as reference, 1 slot) */
                clazz->fields[0].name = strdup("nativePeer");
                clazz->fields[0].descriptor = strdup("Ljava/lang/Object;");  /* reference - 1 slot */
                clazz->fields[0].name_index = add_utf8(clazz, "nativePeer");
                clazz->fields[0].descriptor_index = add_utf8(clazz, "Ljava/lang/Object;");
                clazz->fields[0].access_flags = ACC_PRIVATE;
                clazz->instance_size = sizeof(ObjectHeader) + sizeof(JavaValue);  /* 1 slot */
            }
            CLASS_DEBUG(" Added nativePeer field to javax/microedition/lcdui/Image\n");
        }
        
        /* javax.microedition.lcdui.Graphics needs native pointer field */
        else if (clazz->class_name && strcmp(clazz->class_name, "javax/microedition/lcdui/Graphics") == 0 && clazz->fields_count == 0) {
            clazz->fields_count = 1;
            clazz->fields = (JavaField*)calloc(1, sizeof(JavaField));
            if (clazz->fields) {
                /* Field 0: native pointer (stored as reference, 1 slot) */
                clazz->fields[0].name = strdup("nativePeer");
                clazz->fields[0].descriptor = strdup("Ljava/lang/Object;");  /* reference - 1 slot */
                clazz->fields[0].name_index = add_utf8(clazz, "nativePeer");
                clazz->fields[0].descriptor_index = add_utf8(clazz, "Ljava/lang/Object;");
                clazz->fields[0].access_flags = ACC_PRIVATE;
                clazz->instance_size = sizeof(ObjectHeader) + sizeof(JavaValue);  /* 1 slot */
            }
            CLASS_DEBUG(" Added nativePeer field to javax/microedition/lcdui/Graphics\n");
        }
        
        /* javax.microedition.lcdui.Font needs native pointer field */
        else if (clazz->class_name && strcmp(clazz->class_name, "javax/microedition/lcdui/Font") == 0 && clazz->fields_count == 0) {
            clazz->fields_count = 1;
            clazz->fields = (JavaField*)calloc(1, sizeof(JavaField));
            if (clazz->fields) {
                /* Field 0: native pointer (stored as reference, 1 slot) */
                clazz->fields[0].name = strdup("nativePeer");
                clazz->fields[0].descriptor = strdup("Ljava/lang/Object;");  /* reference - 1 slot */
                clazz->fields[0].name_index = add_utf8(clazz, "nativePeer");
                clazz->fields[0].descriptor_index = add_utf8(clazz, "Ljava/lang/Object;");
                clazz->fields[0].access_flags = ACC_PRIVATE;
                clazz->instance_size = sizeof(ObjectHeader) + sizeof(JavaValue);  /* 1 slot */
            }
            CLASS_DEBUG(" Added nativePeer field to javax/microedition/lcdui/Font\n");
        }
        
        /* javax.microedition.lcdui.game.Layer - abstract base class with x, y, width, height, visible */
        else if (clazz->class_name && strcmp(clazz->class_name, "javax/microedition/lcdui/game/Layer") == 0 && clazz->fields_count == 0) {
            clazz->fields_count = 5;
            clazz->fields = (JavaField*)calloc(5, sizeof(JavaField));
            if (clazz->fields) {
                /* Field 0: x (int) */
                clazz->fields[0].name = strdup("x");
                clazz->fields[0].descriptor = strdup("I");
                clazz->fields[0].name_index = add_utf8(clazz, "x");
                clazz->fields[0].descriptor_index = add_utf8(clazz, "I");
                clazz->fields[0].access_flags = ACC_PRIVATE;
                
                /* Field 1: y (int) */
                clazz->fields[1].name = strdup("y");
                clazz->fields[1].descriptor = strdup("I");
                clazz->fields[1].name_index = add_utf8(clazz, "y");
                clazz->fields[1].descriptor_index = add_utf8(clazz, "I");
                clazz->fields[1].access_flags = ACC_PRIVATE;
                
                /* Field 2: width (int) */
                clazz->fields[2].name = strdup("width");
                clazz->fields[2].descriptor = strdup("I");
                clazz->fields[2].name_index = add_utf8(clazz, "width");
                clazz->fields[2].descriptor_index = add_utf8(clazz, "I");
                clazz->fields[2].access_flags = ACC_PRIVATE;
                
                /* Field 3: height (int) */
                clazz->fields[3].name = strdup("height");
                clazz->fields[3].descriptor = strdup("I");
                clazz->fields[3].name_index = add_utf8(clazz, "height");
                clazz->fields[3].descriptor_index = add_utf8(clazz, "I");
                clazz->fields[3].access_flags = ACC_PRIVATE;
                
                /* Field 4: visible (boolean) */
                clazz->fields[4].name = strdup("visible");
                clazz->fields[4].descriptor = strdup("Z");
                clazz->fields[4].name_index = add_utf8(clazz, "visible");
                clazz->fields[4].descriptor_index = add_utf8(clazz, "Z");
                clazz->fields[4].access_flags = ACC_PRIVATE;
                
                clazz->instance_size = sizeof(ObjectHeader) + sizeof(JavaValue) * 5;
            }
            CLASS_DEBUG(" Added fields to javax/microedition/lcdui/game/Layer\n");
        }
        
        /* javax.microedition.lcdui.game.Sprite - extends Layer, adds own fields
         * ИСПРАВЛЕНО: Не дублируем унаследованные поля! Sprite имеет только свои поля.
         * Поля Layer (x, y, width, height, visible) уже есть в суперклассе.
         * instance_size рассчитывается как: super->instance_size + собственные поля.
         */
        else if (clazz->class_name && strcmp(clazz->class_name, "javax/microedition/lcdui/game/Sprite") == 0) {
            /* Sprite adds 13 fields: image, frameWidth, frameHeight, currentFrame, transform,
             * rawFrameCount, frameSequence, refX, refY,
             * collisionX, collisionY, collisionWidth, collisionHeight */
            clazz->fields_count = 13;
            clazz->fields = (JavaField*)calloc(13, sizeof(JavaField));
            if (clazz->fields) {
                /* Field 0: image (Image) */
                clazz->fields[0].name = strdup("image");
                clazz->fields[0].descriptor = strdup("Ljavax/microedition/lcdui/Image;");
                clazz->fields[0].name_index = add_utf8(clazz, "image");
                clazz->fields[0].descriptor_index = add_utf8(clazz, "Ljavax/microedition/lcdui/Image;");
                clazz->fields[0].access_flags = ACC_PRIVATE;
                
                /* Field 1: frameWidth (int) */
                clazz->fields[1].name = strdup("frameWidth");
                clazz->fields[1].descriptor = strdup("I");
                clazz->fields[1].name_index = add_utf8(clazz, "frameWidth");
                clazz->fields[1].descriptor_index = add_utf8(clazz, "I");
                clazz->fields[1].access_flags = ACC_PRIVATE;
                
                /* Field 2: frameHeight (int) */
                clazz->fields[2].name = strdup("frameHeight");
                clazz->fields[2].descriptor = strdup("I");
                clazz->fields[2].name_index = add_utf8(clazz, "frameHeight");
                clazz->fields[2].descriptor_index = add_utf8(clazz, "I");
                clazz->fields[2].access_flags = ACC_PRIVATE;
                
                /* Field 3: currentFrame (int) */
                clazz->fields[3].name = strdup("currentFrame");
                clazz->fields[3].descriptor = strdup("I");
                clazz->fields[3].name_index = add_utf8(clazz, "currentFrame");
                clazz->fields[3].descriptor_index = add_utf8(clazz, "I");
                clazz->fields[3].access_flags = ACC_PRIVATE;
                
                /* Field 4: transform (int) */
                clazz->fields[4].name = strdup("transform");
                clazz->fields[4].descriptor = strdup("I");
                clazz->fields[4].name_index = add_utf8(clazz, "transform");
                clazz->fields[4].descriptor_index = add_utf8(clazz, "I");
                clazz->fields[4].access_flags = ACC_PRIVATE;
                
                /* Field 5: rawFrameCount (int) */
                clazz->fields[5].name = strdup("rawFrameCount");
                clazz->fields[5].descriptor = strdup("I");
                clazz->fields[5].name_index = add_utf8(clazz, "rawFrameCount");
                clazz->fields[5].descriptor_index = add_utf8(clazz, "I");
                clazz->fields[5].access_flags = ACC_PRIVATE;
                
                /* Field 6: frameSequence (int[]) */
                clazz->fields[6].name = strdup("frameSequence");
                clazz->fields[6].descriptor = strdup("[I");
                clazz->fields[6].name_index = add_utf8(clazz, "frameSequence");
                clazz->fields[6].descriptor_index = add_utf8(clazz, "[I");
                clazz->fields[6].access_flags = ACC_PRIVATE;
                
                /* Field 7: refX (int) */
                clazz->fields[7].name = strdup("refX");
                clazz->fields[7].descriptor = strdup("I");
                clazz->fields[7].name_index = add_utf8(clazz, "refX");
                clazz->fields[7].descriptor_index = add_utf8(clazz, "I");
                clazz->fields[7].access_flags = ACC_PRIVATE;
                
                /* Field 8: refY (int) */
                clazz->fields[8].name = strdup("refY");
                clazz->fields[8].descriptor = strdup("I");
                clazz->fields[8].name_index = add_utf8(clazz, "refY");
                clazz->fields[8].descriptor_index = add_utf8(clazz, "I");
                clazz->fields[8].access_flags = ACC_PRIVATE;
                
                /* Field 9: collisionX (int) */
                clazz->fields[9].name = strdup("collisionX");
                clazz->fields[9].descriptor = strdup("I");
                clazz->fields[9].name_index = add_utf8(clazz, "collisionX");
                clazz->fields[9].descriptor_index = add_utf8(clazz, "I");
                clazz->fields[9].access_flags = ACC_PRIVATE;
                
                /* Field 10: collisionY (int) */
                clazz->fields[10].name = strdup("collisionY");
                clazz->fields[10].descriptor = strdup("I");
                clazz->fields[10].name_index = add_utf8(clazz, "collisionY");
                clazz->fields[10].descriptor_index = add_utf8(clazz, "I");
                clazz->fields[10].access_flags = ACC_PRIVATE;
                
                /* Field 11: collisionWidth (int) */
                clazz->fields[11].name = strdup("collisionWidth");
                clazz->fields[11].descriptor = strdup("I");
                clazz->fields[11].name_index = add_utf8(clazz, "collisionWidth");
                clazz->fields[11].descriptor_index = add_utf8(clazz, "I");
                clazz->fields[11].access_flags = ACC_PRIVATE;
                
                /* Field 12: collisionHeight (int) */
                clazz->fields[12].name = strdup("collisionHeight");
                clazz->fields[12].descriptor = strdup("I");
                clazz->fields[12].name_index = add_utf8(clazz, "collisionHeight");
                clazz->fields[12].descriptor_index = add_utf8(clazz, "I");
                clazz->fields[12].access_flags = ACC_PRIVATE;
                
                /* instance_size = Layer's 5 fields + Sprite's 13 fields = 18 slots */
                clazz->instance_size = sizeof(ObjectHeader) + sizeof(JavaValue) * 18;
            }
            CLASS_DEBUG(" Added fields to javax/microedition/lcdui/game/Sprite\n");
        }
        
        /* javax.microedition.lcdui.game.TiledLayer - extends Layer
         * ИСПРАВЛЕНО: Не дублируем унаследованные поля! TiledLayer имеет только свои поля.
         * Поля Layer (x, y, width, height, visible) уже есть в суперклассе.
         */
        else if (clazz->class_name && strcmp(clazz->class_name, "javax/microedition/lcdui/game/TiledLayer") == 0) {
            /* TiledLayer adds 8 own fields:
             * columns, rows, image, tileWidth, tileHeight,
             * cellMap, animatedTiles, animatedTileCount */
            clazz->fields_count = 8;
            clazz->fields = (JavaField*)calloc(8, sizeof(JavaField));
            if (clazz->fields) {
                /* TiledLayer-specific fields (NOT inherited from Layer) */
                clazz->fields[0].name = strdup("columns");
                clazz->fields[0].descriptor = strdup("I");
                clazz->fields[0].access_flags = ACC_PRIVATE;
                
                clazz->fields[1].name = strdup("rows");
                clazz->fields[1].descriptor = strdup("I");
                clazz->fields[1].access_flags = ACC_PRIVATE;
                
                clazz->fields[2].name = strdup("image");
                clazz->fields[2].descriptor = strdup("Ljavax/microedition/lcdui/Image;");
                clazz->fields[2].access_flags = ACC_PRIVATE;
                
                clazz->fields[3].name = strdup("tileWidth");
                clazz->fields[3].descriptor = strdup("I");
                clazz->fields[3].access_flags = ACC_PRIVATE;
                
                clazz->fields[4].name = strdup("tileHeight");
                clazz->fields[4].descriptor = strdup("I");
                clazz->fields[4].access_flags = ACC_PRIVATE;
                
                /* Cell map: int[] storing tile index for each cell */
                clazz->fields[5].name = strdup("cellMap");
                clazz->fields[5].descriptor = strdup("[I");
                clazz->fields[5].access_flags = ACC_PRIVATE;
                
                /* Animated tiles: int[] mapping animated tile index -> static tile index */
                clazz->fields[6].name = strdup("animatedTiles");
                clazz->fields[6].descriptor = strdup("[I");
                clazz->fields[6].access_flags = ACC_PRIVATE;
                
                /* Number of animated tiles created */
                clazz->fields[7].name = strdup("animatedTileCount");
                clazz->fields[7].descriptor = strdup("I");
                clazz->fields[7].access_flags = ACC_PRIVATE;
                
                /* instance_size = Layer's 5 fields + TiledLayer's 8 fields = 13 slots */
                clazz->instance_size = sizeof(ObjectHeader) + sizeof(JavaValue) * 13;
            }
            CLASS_DEBUG(" Added fields to javax/microedition/lcdui/game/TiledLayer\n");
        }
        
        /* javax.microedition.lcdui.game.LayerManager */
        else if (clazz->class_name && strcmp(clazz->class_name, "javax/microedition/lcdui/game/LayerManager") == 0) {
            /* LayerManager needs: layers array, viewX, viewY, viewWidth, viewHeight */
            clazz->fields_count = 6;
            clazz->fields = (JavaField*)calloc(6, sizeof(JavaField));
            if (clazz->fields) {
                /* Field 0: layers (Layer[]) */
                clazz->fields[0].name = strdup("layers");
                clazz->fields[0].descriptor = strdup("[Ljavax/microedition/lcdui/game/Layer;");
                clazz->fields[0].access_flags = ACC_PRIVATE;
                
                clazz->fields[1].name = strdup("viewX");
                clazz->fields[1].descriptor = strdup("I");
                clazz->fields[1].access_flags = ACC_PRIVATE;
                
                clazz->fields[2].name = strdup("viewY");
                clazz->fields[2].descriptor = strdup("I");
                clazz->fields[2].access_flags = ACC_PRIVATE;
                
                clazz->fields[3].name = strdup("viewWidth");
                clazz->fields[3].descriptor = strdup("I");
                clazz->fields[3].access_flags = ACC_PRIVATE;
                
                clazz->fields[4].name = strdup("viewHeight");
                clazz->fields[4].descriptor = strdup("I");
                clazz->fields[4].access_flags = ACC_PRIVATE;
                
                clazz->fields[5].name = strdup("layerCount");
                clazz->fields[5].descriptor = strdup("I");
                clazz->fields[5].access_flags = ACC_PRIVATE;
                
                clazz->instance_size = sizeof(ObjectHeader) + sizeof(JavaValue) * 6;
            }
            CLASS_DEBUG(" Added fields to javax/microedition/lcdui/game/LayerManager\n");
        }
        
        /* java.lang.Thread needs target, name, priority fields */
        else if (clazz->class_name && strcmp(clazz->class_name, "java/lang/Thread") == 0) {
            clazz->fields_count = 3;
            clazz->fields = (JavaField*)calloc(3, sizeof(JavaField));
            if (clazz->fields) {
                /* Field 0: target (Runnable) */
                clazz->fields[0].name = strdup("target");
                clazz->fields[0].descriptor = strdup("Ljava/lang/Runnable;");
                clazz->fields[0].name_index = add_utf8(clazz, "target");
                clazz->fields[0].descriptor_index = add_utf8(clazz, "Ljava/lang/Runnable;");
                clazz->fields[0].access_flags = ACC_PRIVATE;
                
                /* Field 1: name (String) */
                clazz->fields[1].name = strdup("name");
                clazz->fields[1].descriptor = strdup("Ljava/lang/String;");
                clazz->fields[1].name_index = add_utf8(clazz, "name");
                clazz->fields[1].descriptor_index = add_utf8(clazz, "Ljava/lang/String;");
                clazz->fields[1].access_flags = ACC_PRIVATE;
                
                /* Field 2: priority (int) */
                clazz->fields[2].name = strdup("priority");
                clazz->fields[2].descriptor = strdup("I");
                clazz->fields[2].name_index = add_utf8(clazz, "priority");
                clazz->fields[2].descriptor_index = add_utf8(clazz, "I");
                clazz->fields[2].access_flags = ACC_PRIVATE;
                
                clazz->instance_size = sizeof(ObjectHeader) + sizeof(JavaValue) * 3;
            }
            CLASS_DEBUG(" Added target/name/priority fields to java/lang/Thread\n");
        }
        
        /* java.io.ByteArrayInputStream needs buffer and position fields */
        else if (clazz->class_name && strcmp(clazz->class_name, "java/io/ByteArrayInputStream") == 0) {
            /* Fields: [0] = buf (byte[]), [1] = pos (int), [2] = count (int),
             *         [3] = mark (int) — v20: needed by DataInputStream.mark/reset */
            clazz->fields_count = 4;
            clazz->fields = (JavaField*)calloc(4, sizeof(JavaField));
            if (clazz->fields) {
                /* Field 0: buf (byte[]) */
                clazz->fields[0].name = strdup("buf");
                clazz->fields[0].descriptor = strdup("[B");  /* byte array */
                clazz->fields[0].name_index = add_utf8(clazz, "buf");
                clazz->fields[0].descriptor_index = add_utf8(clazz, "[B");
                clazz->fields[0].access_flags = ACC_PROTECTED;

                /* Field 1: pos (int) */
                clazz->fields[1].name = strdup("pos");
                clazz->fields[1].descriptor = strdup("I");
                clazz->fields[1].name_index = add_utf8(clazz, "pos");
                clazz->fields[1].descriptor_index = add_utf8(clazz, "I");
                clazz->fields[1].access_flags = ACC_PROTECTED;

                /* Field 2: count (int) */
                clazz->fields[2].name = strdup("count");
                clazz->fields[2].descriptor = strdup("I");
                clazz->fields[2].name_index = add_utf8(clazz, "count");
                clazz->fields[2].descriptor_index = add_utf8(clazz, "I");
                clazz->fields[2].access_flags = ACC_PROTECTED;

                /* Field 3: mark (int) */
                clazz->fields[3].name = strdup("mark");
                clazz->fields[3].descriptor = strdup("I");
                clazz->fields[3].name_index = add_utf8(clazz, "mark");
                clazz->fields[3].descriptor_index = add_utf8(clazz, "I");
                clazz->fields[3].access_flags = ACC_PROTECTED;

                clazz->instance_size = sizeof(ObjectHeader) + sizeof(JavaValue) * 4;  /* Four JavaValue slots */
            }
            CLASS_DEBUG(" Added buf/pos/count/mark fields to java/io/ByteArrayInputStream\n");
        }
        
        /* java.io.ByteArrayOutputStream needs buffer and count fields */
        else if (clazz->class_name && strcmp(clazz->class_name, "java/io/ByteArrayOutputStream") == 0) {
            /* Fields: [0] = buf (byte[]), [1] = count (int) */
            clazz->fields_count = 2;
            clazz->fields = (JavaField*)calloc(2, sizeof(JavaField));
            if (clazz->fields) {
                /* Field 0: buf (byte[]) */
                clazz->fields[0].name = strdup("buf");
                clazz->fields[0].descriptor = strdup("[B");  /* byte array */
                clazz->fields[0].name_index = add_utf8(clazz, "buf");
                clazz->fields[0].descriptor_index = add_utf8(clazz, "[B");
                clazz->fields[0].access_flags = ACC_PROTECTED;

                /* Field 1: count (int) */
                clazz->fields[1].name = strdup("count");
                clazz->fields[1].descriptor = strdup("I");
                clazz->fields[1].name_index = add_utf8(clazz, "count");
                clazz->fields[1].descriptor_index = add_utf8(clazz, "I");
                clazz->fields[1].access_flags = ACC_PROTECTED;

                clazz->instance_size = sizeof(ObjectHeader) + sizeof(JavaValue) * 2;  /* Two JavaValue slots */
            }
            CLASS_DEBUG(" Added buf/count fields to java/io/ByteArrayOutputStream\n");
        }
        
        /* java.io.InputStreamReader needs 'in' field for underlying InputStream */
        else if (clazz->class_name && strcmp(clazz->class_name, "java/io/InputStreamReader") == 0) {
            /* Fields: [0] = in (InputStream) - reference to underlying InputStream */
            clazz->fields_count = 1;
            clazz->fields = (JavaField*)calloc(1, sizeof(JavaField));
            if (clazz->fields) {
                /* Field 0: in (InputStream reference) */
                clazz->fields[0].name = strdup("in");
                clazz->fields[0].descriptor = strdup("Ljava/io/InputStream;");
                clazz->fields[0].name_index = add_utf8(clazz, "in");
                clazz->fields[0].descriptor_index = add_utf8(clazz, "Ljava/io/InputStream;");
                clazz->fields[0].access_flags = ACC_PROTECTED;

                clazz->instance_size = sizeof(ObjectHeader) + sizeof(JavaValue);  /* One JavaValue slot */
            }
            CLASS_DEBUG(" Added 'in' field to java/io/InputStreamReader\n");
        }
        
        /* java.io.BufferedReader needs in/reader field for underlying Reader */
        else if (clazz->class_name && strcmp(clazz->class_name, "java/io/BufferedReader") == 0) {
            /* Fields: [0] = in (Reader reference) */
            clazz->fields_count = 1;
            clazz->fields = (JavaField*)calloc(1, sizeof(JavaField));
            if (clazz->fields) {
                /* Field 0: in (Reader reference) */
                clazz->fields[0].name = strdup("in");
                clazz->fields[0].descriptor = strdup("Ljava/io/Reader;");
                clazz->fields[0].name_index = add_utf8(clazz, "in");
                clazz->fields[0].descriptor_index = add_utf8(clazz, "Ljava/io/Reader;");
                clazz->fields[0].access_flags = ACC_PROTECTED;

                clazz->instance_size = sizeof(ObjectHeader) + sizeof(JavaValue);  /* One JavaValue slot */
            }
            CLASS_DEBUG(" Added in field to java/io/BufferedReader\n");
        }
        
        /* java.io.DataOutputStream needs out field */
        else if (clazz->class_name && strcmp(clazz->class_name, "java/io/DataOutputStream") == 0) {
            /* Fields: [0] = out (OutputStream) */
            clazz->fields_count = 1;
            clazz->fields = (JavaField*)calloc(1, sizeof(JavaField));
            if (clazz->fields) {
                /* Field 0: out (OutputStream) */
                clazz->fields[0].name = strdup("out");
                clazz->fields[0].descriptor = strdup("Ljava/io/OutputStream;");
                clazz->fields[0].name_index = add_utf8(clazz, "out");
                clazz->fields[0].descriptor_index = add_utf8(clazz, "Ljava/io/OutputStream;");
                clazz->fields[0].access_flags = ACC_PROTECTED;

                clazz->instance_size = sizeof(ObjectHeader) + sizeof(JavaValue);  /* One JavaValue slot */
            }
            CLASS_DEBUG(" Added out field to java/io/DataOutputStream\n");
        }

        /* javax.microedition.rms.RecordStore needs nativeHandle field */
        else if (clazz->class_name && strcmp(clazz->class_name, "javax/microedition/rms/RecordStore") == 0) {
            clazz->fields_count = 1;
            clazz->fields = (JavaField*)calloc(1, sizeof(JavaField));
            if (clazz->fields) {
                /* Field 0: nativeHandle (int) - stores native record store handle */
                clazz->fields[0].name = strdup("nativeHandle");
                clazz->fields[0].descriptor = strdup("I");  /* int */
                clazz->fields[0].name_index = add_utf8(clazz, "nativeHandle");
                clazz->fields[0].descriptor_index = add_utf8(clazz, "I");
                clazz->fields[0].access_flags = ACC_PRIVATE;
                clazz->instance_size = sizeof(ObjectHeader) + sizeof(JavaValue);  /* One JavaValue slot */
            }
            CLASS_DEBUG(" Added nativeHandle field to javax/microedition/rms/RecordStore\n");
        }
        
        /* javax.microedition.lcdui.Form needs title and items fields */
        else if (clazz->class_name && strcmp(clazz->class_name, "javax/microedition/lcdui/Form") == 0) {
            /* Fields: [0] = title (String), [1] = items (Item[]) */
            clazz->fields_count = 2;
            clazz->fields = (JavaField*)calloc(2, sizeof(JavaField));
            if (clazz->fields) {
                clazz->fields[0].name = strdup("title");
                clazz->fields[0].descriptor = strdup("Ljava/lang/String;");
                clazz->fields[0].name_index = add_utf8(clazz, "title");
                clazz->fields[0].descriptor_index = add_utf8(clazz, "Ljava/lang/String;");
                clazz->fields[0].access_flags = ACC_PRIVATE;
                
                clazz->fields[1].name = strdup("items");
                clazz->fields[1].descriptor = strdup("[Ljavax/microedition/lcdui/Item;");
                clazz->fields[1].name_index = add_utf8(clazz, "items");
                clazz->fields[1].descriptor_index = add_utf8(clazz, "[Ljavax/microedition/lcdui/Item;");
                clazz->fields[1].access_flags = ACC_PRIVATE;
                
                clazz->instance_size = sizeof(ObjectHeader) + sizeof(JavaValue) * 4;
            }
            CLASS_DEBUG(" Added title/items fields to javax/microedition/lcdui/Form\n");
        }
        
        /* javax.microedition.lcdui.List needs title, type, strings, selected, listener fields */
        else if (clazz->class_name && strcmp(clazz->class_name, "javax/microedition/lcdui/List") == 0) {
            clazz->fields_count = 5;
            clazz->fields = (JavaField*)calloc(5, sizeof(JavaField));
            if (clazz->fields) {
                clazz->fields[0].name = strdup("title");
                clazz->fields[0].descriptor = strdup("Ljava/lang/String;");
                clazz->fields[0].name_index = add_utf8(clazz, "title");
                clazz->fields[0].descriptor_index = add_utf8(clazz, "Ljava/lang/String;");
                clazz->fields[0].access_flags = ACC_PRIVATE;
                
                clazz->fields[1].name = strdup("listType");
                clazz->fields[1].descriptor = strdup("I");
                clazz->fields[1].name_index = add_utf8(clazz, "listType");
                clazz->fields[1].descriptor_index = add_utf8(clazz, "I");
                clazz->fields[1].access_flags = ACC_PRIVATE;
                
                clazz->fields[2].name = strdup("strings");
                clazz->fields[2].descriptor = strdup("[Ljava/lang/String;");
                clazz->fields[2].name_index = add_utf8(clazz, "strings");
                clazz->fields[2].descriptor_index = add_utf8(clazz, "[Ljava/lang/String;");
                clazz->fields[2].access_flags = ACC_PRIVATE;
                
                clazz->fields[3].name = strdup("selected");
                clazz->fields[3].descriptor = strdup("[Z");
                clazz->fields[3].name_index = add_utf8(clazz, "selected");
                clazz->fields[3].descriptor_index = add_utf8(clazz, "[Z");
                clazz->fields[3].access_flags = ACC_PRIVATE;
                
                clazz->fields[4].name = strdup("listener");
                clazz->fields[4].descriptor = strdup("Ljavax/microedition/lcdui/CommandListener;");
                clazz->fields[4].name_index = add_utf8(clazz, "listener");
                clazz->fields[4].descriptor_index = add_utf8(clazz, "Ljavax/microedition/lcdui/ChoiceGroupListener;");
                clazz->fields[4].access_flags = ACC_PRIVATE;
                
                /* 7 slots total: 2 from Displayable + 5 own fields */
                clazz->instance_size = sizeof(ObjectHeader) + sizeof(JavaValue) * 7;
            }
            CLASS_DEBUG(" Added fields to javax/microedition/lcdui/List\n");
            
            /* Add static field SELECT_COMMAND */
            clazz->static_fields = (JavaStaticField*)calloc(1, sizeof(JavaStaticField));
            clazz->static_fields_count = 1;
            clazz->static_fields_capacity = 1;
            
            clazz->static_fields[0].name = strdup("SELECT_COMMAND");
            clazz->static_fields[0].descriptor = strdup("Ljavax/microedition/lcdui/Command;");
            memset(&clazz->static_fields[0].value, 0, sizeof(JavaValue));
            
            CLASS_DEBUG(" Added SELECT_COMMAND static field to List\n");
        }
        
        /* javax.microedition.lcdui.Alert needs title, text, image, type, timeout fields */
        else if (clazz->class_name && strcmp(clazz->class_name, "javax/microedition/lcdui/Alert") == 0) {
            clazz->fields_count = 5;
            clazz->fields = (JavaField*)calloc(5, sizeof(JavaField));
            if (clazz->fields) {
                clazz->fields[0].name = strdup("title");
                clazz->fields[0].descriptor = strdup("Ljava/lang/String;");
                clazz->fields[0].name_index = add_utf8(clazz, "title");
                clazz->fields[0].descriptor_index = add_utf8(clazz, "Ljava/lang/String;");
                clazz->fields[0].access_flags = ACC_PRIVATE;
                
                clazz->fields[1].name = strdup("text");
                clazz->fields[1].descriptor = strdup("Ljava/lang/String;");
                clazz->fields[1].name_index = add_utf8(clazz, "text");
                clazz->fields[1].descriptor_index = add_utf8(clazz, "Ljava/lang/String;");
                clazz->fields[1].access_flags = ACC_PRIVATE;
                
                clazz->fields[2].name = strdup("image");
                clazz->fields[2].descriptor = strdup("Ljavax/microedition/lcdui/Image;");
                clazz->fields[2].name_index = add_utf8(clazz, "image");
                clazz->fields[2].descriptor_index = add_utf8(clazz, "Ljavax/microedition/lcdui/Image;");
                clazz->fields[2].access_flags = ACC_PRIVATE;
                
                clazz->fields[3].name = strdup("alertType");
                clazz->fields[3].descriptor = strdup("Ljavax/microedition/lcdui/AlertType;");
                clazz->fields[3].name_index = add_utf8(clazz, "alertType");
                clazz->fields[3].descriptor_index = add_utf8(clazz, "Ljavax/microedition/lcdui/AlertType;");
                clazz->fields[3].access_flags = ACC_PRIVATE;
                
                clazz->fields[4].name = strdup("timeout");
                clazz->fields[4].descriptor = strdup("I");
                clazz->fields[4].name_index = add_utf8(clazz, "timeout");
                clazz->fields[4].descriptor_index = add_utf8(clazz, "I");
                clazz->fields[4].access_flags = ACC_PRIVATE;
                
                /* 7 slots total: 2 from Displayable (commands, listener) + 5 own fields */
                clazz->instance_size = sizeof(ObjectHeader) + sizeof(JavaValue) * 7;
            }
            CLASS_DEBUG(" Added fields to javax/microedition/lcdui/Alert\n");
        }
        
        /* javax.microedition.lcdui.TextBox needs title, text, maxSize, constraints fields */
        else if (clazz->class_name && strcmp(clazz->class_name, "javax/microedition/lcdui/TextBox") == 0) {
            clazz->fields_count = 4;
            clazz->fields = (JavaField*)calloc(4, sizeof(JavaField));
            if (clazz->fields) {
                clazz->fields[0].name = strdup("title");
                clazz->fields[0].descriptor = strdup("Ljava/lang/String;");
                clazz->fields[0].name_index = add_utf8(clazz, "title");
                clazz->fields[0].descriptor_index = add_utf8(clazz, "Ljava/lang/String;");
                clazz->fields[0].access_flags = ACC_PRIVATE;
                
                clazz->fields[1].name = strdup("text");
                clazz->fields[1].descriptor = strdup("Ljava/lang/String;");
                clazz->fields[1].name_index = add_utf8(clazz, "text");
                clazz->fields[1].descriptor_index = add_utf8(clazz, "Ljava/lang/String;");
                clazz->fields[1].access_flags = ACC_PRIVATE;
                
                clazz->fields[2].name = strdup("maxSize");
                clazz->fields[2].descriptor = strdup("I");
                clazz->fields[2].name_index = add_utf8(clazz, "maxSize");
                clazz->fields[2].descriptor_index = add_utf8(clazz, "I");
                clazz->fields[2].access_flags = ACC_PRIVATE;
                
                clazz->fields[3].name = strdup("constraints");
                clazz->fields[3].descriptor = strdup("I");
                clazz->fields[3].name_index = add_utf8(clazz, "constraints");
                clazz->fields[3].descriptor_index = add_utf8(clazz, "I");
                clazz->fields[3].access_flags = ACC_PRIVATE;
                
                clazz->instance_size = sizeof(ObjectHeader) + sizeof(JavaValue) * 6;
            }
            CLASS_DEBUG(" Added fields to javax/microedition/lcdui/TextBox\n");
        }
        
        /* javax.microedition.lcdui.Item needs label field */
        else if (clazz->class_name && strcmp(clazz->class_name, "javax/microedition/lcdui/Item") == 0) {
            clazz->fields_count = 1;
            clazz->fields = (JavaField*)calloc(1, sizeof(JavaField));
            if (clazz->fields) {
                clazz->fields[0].name = strdup("label");
                clazz->fields[0].descriptor = strdup("Ljava/lang/String;");
                clazz->fields[0].name_index = add_utf8(clazz, "label");
                clazz->fields[0].descriptor_index = add_utf8(clazz, "Ljava/lang/String;");
                clazz->fields[0].access_flags = ACC_PRIVATE;
                clazz->instance_size = sizeof(ObjectHeader) + sizeof(JavaValue);
            }
            CLASS_DEBUG(" Added label field to javax/microedition/lcdui/Item\n");
        }
        
        /* javax.microedition.lcdui.StringItem needs label, text fields */
        else if (clazz->class_name && strcmp(clazz->class_name, "javax/microedition/lcdui/StringItem") == 0) {
            clazz->fields_count = 2;
            clazz->fields = (JavaField*)calloc(2, sizeof(JavaField));
            if (clazz->fields) {
                clazz->fields[0].name = strdup("label");
                clazz->fields[0].descriptor = strdup("Ljava/lang/String;");
                clazz->fields[0].name_index = add_utf8(clazz, "label");
                clazz->fields[0].descriptor_index = add_utf8(clazz, "Ljava/lang/String;");
                clazz->fields[0].access_flags = ACC_PRIVATE;
                
                clazz->fields[1].name = strdup("text");
                clazz->fields[1].descriptor = strdup("Ljava/lang/String;");
                clazz->fields[1].name_index = add_utf8(clazz, "text");
                clazz->fields[1].descriptor_index = add_utf8(clazz, "Ljava/lang/String;");
                clazz->fields[1].access_flags = ACC_PRIVATE;
                
                clazz->instance_size = sizeof(ObjectHeader) + sizeof(JavaValue) * 2;
            }
            CLASS_DEBUG(" Added fields to javax/microedition/lcdui/StringItem\n");
        }
        
        /* javax.microedition.lcdui.TextField needs label, text, maxSize, constraints fields */
        else if (clazz->class_name && strcmp(clazz->class_name, "javax/microedition/lcdui/TextField") == 0) {
            clazz->fields_count = 4;
            clazz->fields = (JavaField*)calloc(4, sizeof(JavaField));
            if (clazz->fields) {
                clazz->fields[0].name = strdup("label");
                clazz->fields[0].descriptor = strdup("Ljava/lang/String;");
                clazz->fields[0].name_index = add_utf8(clazz, "label");
                clazz->fields[0].descriptor_index = add_utf8(clazz, "Ljava/lang/String;");
                clazz->fields[0].access_flags = ACC_PRIVATE;
                
                clazz->fields[1].name = strdup("text");
                clazz->fields[1].descriptor = strdup("Ljava/lang/String;");
                clazz->fields[1].name_index = add_utf8(clazz, "text");
                clazz->fields[1].descriptor_index = add_utf8(clazz, "Ljava/lang/String;");
                clazz->fields[1].access_flags = ACC_PRIVATE;
                
                clazz->fields[2].name = strdup("maxSize");
                clazz->fields[2].descriptor = strdup("I");
                clazz->fields[2].name_index = add_utf8(clazz, "maxSize");
                clazz->fields[2].descriptor_index = add_utf8(clazz, "I");
                clazz->fields[2].access_flags = ACC_PRIVATE;
                
                clazz->fields[3].name = strdup("constraints");
                clazz->fields[3].descriptor = strdup("I");
                clazz->fields[3].name_index = add_utf8(clazz, "constraints");
                clazz->fields[3].descriptor_index = add_utf8(clazz, "I");
                clazz->fields[3].access_flags = ACC_PRIVATE;
                
                clazz->instance_size = sizeof(ObjectHeader) + sizeof(JavaValue) * 4;
            }
            CLASS_DEBUG(" Added fields to javax/microedition/lcdui/TextField\n");
        }
        
        /* javax.microedition.lcdui.ChoiceGroup needs label, type, strings, selected fields */
        else if (clazz->class_name && strcmp(clazz->class_name, "javax/microedition/lcdui/ChoiceGroup") == 0) {
            clazz->fields_count = 4;
            clazz->fields = (JavaField*)calloc(4, sizeof(JavaField));
            if (clazz->fields) {
                clazz->fields[0].name = strdup("label");
                clazz->fields[0].descriptor = strdup("Ljava/lang/String;");
                clazz->fields[0].name_index = add_utf8(clazz, "label");
                clazz->fields[0].descriptor_index = add_utf8(clazz, "Ljava/lang/String;");
                clazz->fields[0].access_flags = ACC_PRIVATE;
                
                clazz->fields[1].name = strdup("choiceType");
                clazz->fields[1].descriptor = strdup("I");
                clazz->fields[1].name_index = add_utf8(clazz, "choiceType");
                clazz->fields[1].descriptor_index = add_utf8(clazz, "I");
                clazz->fields[1].access_flags = ACC_PRIVATE;
                
                clazz->fields[2].name = strdup("strings");
                clazz->fields[2].descriptor = strdup("[Ljava/lang/String;");
                clazz->fields[2].name_index = add_utf8(clazz, "strings");
                clazz->fields[2].descriptor_index = add_utf8(clazz, "[Ljava/lang/String;");
                clazz->fields[2].access_flags = ACC_PRIVATE;
                
                clazz->fields[3].name = strdup("selected");
                clazz->fields[3].descriptor = strdup("[Z");
                clazz->fields[3].name_index = add_utf8(clazz, "selected");
                clazz->fields[3].descriptor_index = add_utf8(clazz, "[Z");
                clazz->fields[3].access_flags = ACC_PRIVATE;
                
                clazz->instance_size = sizeof(ObjectHeader) + sizeof(JavaValue) * 4;
            }
            CLASS_DEBUG(" Added fields to javax/microedition/lcdui/ChoiceGroup\n");
        }
        
        /* javax.microedition.lcdui.Gauge needs label, interactive, max, value fields */
        else if (clazz->class_name && strcmp(clazz->class_name, "javax/microedition/lcdui/Gauge") == 0) {
            clazz->fields_count = 4;
            clazz->fields = (JavaField*)calloc(4, sizeof(JavaField));
            if (clazz->fields) {
                clazz->fields[0].name = strdup("label");
                clazz->fields[0].descriptor = strdup("Ljava/lang/String;");
                clazz->fields[0].name_index = add_utf8(clazz, "label");
                clazz->fields[0].descriptor_index = add_utf8(clazz, "Ljava/lang/String;");
                clazz->fields[0].access_flags = ACC_PRIVATE;
                
                clazz->fields[1].name = strdup("interactive");
                clazz->fields[1].descriptor = strdup("Z");
                clazz->fields[1].name_index = add_utf8(clazz, "interactive");
                clazz->fields[1].descriptor_index = add_utf8(clazz, "Z");
                clazz->fields[1].access_flags = ACC_PRIVATE;
                
                clazz->fields[2].name = strdup("maxValue");
                clazz->fields[2].descriptor = strdup("I");
                clazz->fields[2].name_index = add_utf8(clazz, "maxValue");
                clazz->fields[2].descriptor_index = add_utf8(clazz, "I");
                clazz->fields[2].access_flags = ACC_PRIVATE;
                
                clazz->fields[3].name = strdup("value");
                clazz->fields[3].descriptor = strdup("I");
                clazz->fields[3].name_index = add_utf8(clazz, "value");
                clazz->fields[3].descriptor_index = add_utf8(clazz, "I");
                clazz->fields[3].access_flags = ACC_PRIVATE;
                
                clazz->instance_size = sizeof(ObjectHeader) + sizeof(JavaValue) * 4;
            }
            CLASS_DEBUG(" Added fields to javax/microedition/lcdui/Gauge\n");
        }
        
        /* javax.microedition.lcdui.Spacer needs width, height fields */
        else if (clazz->class_name && strcmp(clazz->class_name, "javax/microedition/lcdui/Spacer") == 0) {
            clazz->fields_count = 2;
            clazz->fields = (JavaField*)calloc(2, sizeof(JavaField));
            if (clazz->fields) {
                clazz->fields[0].name = strdup("width");
                clazz->fields[0].descriptor = strdup("I");
                clazz->fields[0].name_index = add_utf8(clazz, "width");
                clazz->fields[0].descriptor_index = add_utf8(clazz, "I");
                clazz->fields[0].access_flags = ACC_PRIVATE;
                
                clazz->fields[1].name = strdup("height");
                clazz->fields[1].descriptor = strdup("I");
                clazz->fields[1].name_index = add_utf8(clazz, "height");
                clazz->fields[1].descriptor_index = add_utf8(clazz, "I");
                clazz->fields[1].access_flags = ACC_PRIVATE;
                
                clazz->instance_size = sizeof(ObjectHeader) + sizeof(JavaValue) * 2;
            }
            CLASS_DEBUG(" Added fields to javax/microedition/lcdui/Spacer\n");
        }
        
        /* javax.microedition.lcdui.ImageItem needs label, image, layout, altText fields */
        else if (clazz->class_name && strcmp(clazz->class_name, "javax/microedition/lcdui/ImageItem") == 0) {
            clazz->fields_count = 4;
            clazz->fields = (JavaField*)calloc(4, sizeof(JavaField));
            if (clazz->fields) {
                clazz->fields[0].name = strdup("label");
                clazz->fields[0].descriptor = strdup("Ljava/lang/String;");
                clazz->fields[0].name_index = add_utf8(clazz, "label");
                clazz->fields[0].descriptor_index = add_utf8(clazz, "Ljava/lang/String;");
                clazz->fields[0].access_flags = ACC_PRIVATE;
                
                clazz->fields[1].name = strdup("image");
                clazz->fields[1].descriptor = strdup("Ljavax/microedition/lcdui/Image;");
                clazz->fields[1].name_index = add_utf8(clazz, "image");
                clazz->fields[1].descriptor_index = add_utf8(clazz, "Ljavax/microedition/lcdui/Image;");
                clazz->fields[1].access_flags = ACC_PRIVATE;
                
                clazz->fields[2].name = strdup("layout");
                clazz->fields[2].descriptor = strdup("I");
                clazz->fields[2].name_index = add_utf8(clazz, "layout");
                clazz->fields[2].descriptor_index = add_utf8(clazz, "I");
                clazz->fields[2].access_flags = ACC_PRIVATE;
                
                clazz->fields[3].name = strdup("altText");
                clazz->fields[3].descriptor = strdup("Ljava/lang/String;");
                clazz->fields[3].name_index = add_utf8(clazz, "altText");
                clazz->fields[3].descriptor_index = add_utf8(clazz, "Ljava/lang/String;");
                clazz->fields[3].access_flags = ACC_PRIVATE;
                
                clazz->instance_size = sizeof(ObjectHeader) + sizeof(JavaValue) * 4;
            }
            CLASS_DEBUG(" Added fields to javax/microedition/lcdui/ImageItem\n");
        }
        
        /* javax.microedition.lcdui.DateField needs label, mode, date fields */
        else if (clazz->class_name && strcmp(clazz->class_name, "javax/microedition/lcdui/DateField") == 0) {
            clazz->fields_count = 3;
            clazz->fields = (JavaField*)calloc(3, sizeof(JavaField));
            if (clazz->fields) {
                clazz->fields[0].name = strdup("label");
                clazz->fields[0].descriptor = strdup("Ljava/lang/String;");
                clazz->fields[0].name_index = add_utf8(clazz, "label");
                clazz->fields[0].descriptor_index = add_utf8(clazz, "Ljava/lang/String;");
                clazz->fields[0].access_flags = ACC_PRIVATE;
                
                clazz->fields[1].name = strdup("mode");
                clazz->fields[1].descriptor = strdup("I");
                clazz->fields[1].name_index = add_utf8(clazz, "mode");
                clazz->fields[1].descriptor_index = add_utf8(clazz, "I");
                clazz->fields[1].access_flags = ACC_PRIVATE;
                
                clazz->fields[2].name = strdup("date");
                clazz->fields[2].descriptor = strdup("J");  /* long */
                clazz->fields[2].name_index = add_utf8(clazz, "date");
                clazz->fields[2].descriptor_index = add_utf8(clazz, "J");
                clazz->fields[2].access_flags = ACC_PRIVATE;
                
                clazz->instance_size = sizeof(ObjectHeader) + sizeof(JavaValue) * 3;
            }
            CLASS_DEBUG(" Added fields to javax/microedition/lcdui/DateField\n");
        }
        
        /* javax.microedition.lcdui.Ticker needs text field */
        else if (clazz->class_name && strcmp(clazz->class_name, "javax/microedition/lcdui/Ticker") == 0) {
            clazz->fields_count = 1;
            clazz->fields = (JavaField*)calloc(1, sizeof(JavaField));
            if (clazz->fields) {
                clazz->fields[0].name = strdup("text");
                clazz->fields[0].descriptor = strdup("Ljava/lang/String;");
                clazz->fields[0].name_index = add_utf8(clazz, "text");
                clazz->fields[0].descriptor_index = add_utf8(clazz, "Ljava/lang/String;");
                clazz->fields[0].access_flags = ACC_PRIVATE;
                clazz->instance_size = sizeof(ObjectHeader) + sizeof(JavaValue);
            }
            CLASS_DEBUG(" Added text field to javax/microedition/lcdui/Ticker\n");
        }
        
        /* javax.microedition.lcdui.Command needs label, type, priority fields */
        else if (clazz->class_name && strcmp(clazz->class_name, "javax/microedition/lcdui/Command") == 0) {
            clazz->fields_count = 3;
            clazz->fields = (JavaField*)calloc(3, sizeof(JavaField));
            if (clazz->fields) {
                clazz->fields[0].name = strdup("label");
                clazz->fields[0].descriptor = strdup("Ljava/lang/String;");
                clazz->fields[0].name_index = add_utf8(clazz, "label");
                clazz->fields[0].descriptor_index = add_utf8(clazz, "Ljava/lang/String;");
                clazz->fields[0].access_flags = ACC_PRIVATE;
                
                clazz->fields[1].name = strdup("type");
                clazz->fields[1].descriptor = strdup("I");
                clazz->fields[1].name_index = add_utf8(clazz, "type");
                clazz->fields[1].descriptor_index = add_utf8(clazz, "I");
                clazz->fields[1].access_flags = ACC_PRIVATE;
                
                clazz->fields[2].name = strdup("priority");
                clazz->fields[2].descriptor = strdup("I");
                clazz->fields[2].name_index = add_utf8(clazz, "priority");
                clazz->fields[2].descriptor_index = add_utf8(clazz, "I");
                clazz->fields[2].access_flags = ACC_PRIVATE;
                
                clazz->instance_size = sizeof(ObjectHeader) + sizeof(JavaValue) * 3;
            }
            CLASS_DEBUG(" Added fields to javax/microedition/lcdui/Command\n");
        }
        
        /* javax.microedition.lcdui.AlertType - needs static fields INFO, ERROR, WARNING, CONFIRMATION, ALARM */
        else if (clazz->class_name && strcmp(clazz->class_name, "javax/microedition/lcdui/AlertType") == 0) {
            clazz->fields_count = 0;  /* No instance fields */
            clazz->instance_size = sizeof(ObjectHeader);
            
            /* Create 5 static fields for AlertType constants */
            clazz->static_fields = (JavaStaticField*)calloc(5, sizeof(JavaStaticField));
            clazz->static_fields_count = 5;
            clazz->static_fields_capacity = 5;
            if (clazz->static_fields) {
                /* Field 0: INFO */
                clazz->static_fields[0].name = strdup("INFO");
                clazz->static_fields[0].descriptor = strdup("Ljavax/microedition/lcdui/AlertType;");
                memset(&clazz->static_fields[0].value, 0, sizeof(JavaValue));
                
                /* Field 1: ERROR */
                clazz->static_fields[1].name = strdup("ERROR");
                clazz->static_fields[1].descriptor = strdup("Ljavax/microedition/lcdui/AlertType;");
                memset(&clazz->static_fields[1].value, 0, sizeof(JavaValue));
                
                /* Field 2: WARNING */
                clazz->static_fields[2].name = strdup("WARNING");
                clazz->static_fields[2].descriptor = strdup("Ljavax/microedition/lcdui/AlertType;");
                memset(&clazz->static_fields[2].value, 0, sizeof(JavaValue));
                
                /* Field 3: CONFIRMATION */
                clazz->static_fields[3].name = strdup("CONFIRMATION");
                clazz->static_fields[3].descriptor = strdup("Ljavax/microedition/lcdui/AlertType;");
                memset(&clazz->static_fields[3].value, 0, sizeof(JavaValue));
                
                /* Field 4: ALARM */
                clazz->static_fields[4].name = strdup("ALARM");
                clazz->static_fields[4].descriptor = strdup("Ljavax/microedition/lcdui/AlertType;");
                memset(&clazz->static_fields[4].value, 0, sizeof(JavaValue));
            }
            CLASS_DEBUG(" Created javax/microedition/lcdui/AlertType with static fields\n");
        }
        
        /* javax.microedition.lcdui.Displayable needs commands array and listener */
        else if (clazz->class_name && strcmp(clazz->class_name, "javax/microedition/lcdui/Displayable") == 0) {
            clazz->fields_count = 2;
            clazz->fields = (JavaField*)calloc(2, sizeof(JavaField));
            if (clazz->fields) {
                /* Field 0: commands (Command[] array) */
                clazz->fields[0].name = strdup("commands");
                clazz->fields[0].descriptor = strdup("[Ljavax/microedition/lcdui/Command;");
                clazz->fields[0].name_index = add_utf8(clazz, "commands");
                clazz->fields[0].descriptor_index = add_utf8(clazz, "[Ljavax/microedition/lcdui/Command;");
                clazz->fields[0].access_flags = ACC_PRIVATE;
                
                /* Field 1: listener (CommandListener) */
                clazz->fields[1].name = strdup("listener");
                clazz->fields[1].descriptor = strdup("Ljavax/microedition/lcdui/CommandListener;");
                clazz->fields[1].name_index = add_utf8(clazz, "listener");
                clazz->fields[1].descriptor_index = add_utf8(clazz, "Ljavax/microedition/lcdui/CommandListener;");
                clazz->fields[1].access_flags = ACC_PRIVATE;
                
                clazz->instance_size = sizeof(ObjectHeader) + sizeof(JavaValue) * 2;
            }
            CLASS_DEBUG(" Added fields to javax/microedition/lcdui/Displayable\n");
        }
        
        /* com.nokia.mid.sound.Sound needs data, type, state, and soundListener fields */
        else if (clazz->class_name && strcmp(clazz->class_name, "com/nokia/mid/sound/Sound") == 0) {
            clazz->fields_count = 6;
            clazz->fields = (JavaField*)calloc(6, sizeof(JavaField));
            if (clazz->fields) {
                clazz->fields[0].name = strdup("data");
                clazz->fields[0].descriptor = strdup("[B");
                clazz->fields[0].name_index = add_utf8(clazz, "data");
                clazz->fields[0].descriptor_index = add_utf8(clazz, "[B");
                clazz->fields[0].access_flags = ACC_PRIVATE;

                clazz->fields[1].name = strdup("soundType");
                clazz->fields[1].descriptor = strdup("I");
                clazz->fields[1].name_index = add_utf8(clazz, "soundType");
                clazz->fields[1].descriptor_index = add_utf8(clazz, "I");
                clazz->fields[1].access_flags = ACC_PRIVATE;

                clazz->fields[2].name = strdup("state");
                clazz->fields[2].descriptor = strdup("I");
                clazz->fields[2].name_index = add_utf8(clazz, "state");
                clazz->fields[2].descriptor_index = add_utf8(clazz, "I");
                clazz->fields[2].access_flags = ACC_PRIVATE;

                clazz->fields[3].name = strdup("soundListener");
                clazz->fields[3].descriptor = strdup("Lcom/nokia/mid/sound/SoundListener;");
                clazz->fields[3].name_index = add_utf8(clazz, "soundListener");
                clazz->fields[3].descriptor_index = add_utf8(clazz, "Lcom/nokia/mid/sound/SoundListener;");
                clazz->fields[3].access_flags = ACC_PRIVATE;

                /* v17: gain (0..100) and native channel id */
                clazz->fields_count = 6;
                clazz->fields[4].name = strdup("gain");
                clazz->fields[4].descriptor = strdup("I");
                clazz->fields[4].name_index = add_utf8(clazz, "gain");
                clazz->fields[4].descriptor_index = add_utf8(clazz, "I");
                clazz->fields[4].access_flags = ACC_PRIVATE;

                clazz->fields[5].name = strdup("soundId");
                clazz->fields[5].descriptor = strdup("I");
                clazz->fields[5].name_index = add_utf8(clazz, "soundId");
                clazz->fields[5].descriptor_index = add_utf8(clazz, "I");
                clazz->fields[5].access_flags = ACC_PRIVATE;

                clazz->instance_size = sizeof(ObjectHeader) + sizeof(JavaValue) * 6;
            }

            /* Create static final int fields for Nokia Sound constants */
            clazz->static_fields = (JavaStaticField*)calloc(5, sizeof(JavaStaticField));
            clazz->static_fields_count = 5;
            clazz->static_fields_capacity = 5;
            if (clazz->static_fields) {
                clazz->static_fields[0].name = strdup("FORMAT_TONE");
                clazz->static_fields[0].descriptor = strdup("I");
                memset(&clazz->static_fields[0].value, 0, sizeof(JavaValue));
                clazz->static_fields[0].value.i = 1;

                clazz->static_fields[1].name = strdup("FORMAT_WAV");
                clazz->static_fields[1].descriptor = strdup("I");
                memset(&clazz->static_fields[1].value, 0, sizeof(JavaValue));
                clazz->static_fields[1].value.i = 5;

                clazz->static_fields[2].name = strdup("SOUND_PLAYING");
                clazz->static_fields[2].descriptor = strdup("I");
                memset(&clazz->static_fields[2].value, 0, sizeof(JavaValue));
                clazz->static_fields[2].value.i = 0;

                clazz->static_fields[3].name = strdup("SOUND_STOPPED");
                clazz->static_fields[3].descriptor = strdup("I");
                memset(&clazz->static_fields[3].value, 0, sizeof(JavaValue));
                clazz->static_fields[3].value.i = 1;

                clazz->static_fields[4].name = strdup("SOUND_UNINITIALIZED");
                clazz->static_fields[4].descriptor = strdup("I");
                memset(&clazz->static_fields[4].value, 0, sizeof(JavaValue));
                clazz->static_fields[4].value.i = 3;
            }
            CLASS_DEBUG(" Added fields to com/nokia/mid/sound/Sound\n");
        }
        
        /* com.nokia.mid.ui.FullCanvas - Nokia-specific key constants */
        else if (clazz->class_name && strcmp(clazz->class_name, "com/nokia/mid/ui/FullCanvas") == 0) {
            /* No instance fields needed - inherits from Canvas */
            clazz->fields_count = 0;
            clazz->instance_size = sizeof(ObjectHeader);
            
            /* Create 9 static final int fields for Nokia game key constants */
            #define FULLCANVAS_CONST_COUNT 9
            clazz->static_fields = (JavaStaticField*)calloc(FULLCANVAS_CONST_COUNT, sizeof(JavaStaticField));
            clazz->static_fields_count = FULLCANVAS_CONST_COUNT;
            clazz->static_fields_capacity = FULLCANVAS_CONST_COUNT;
            if (clazz->static_fields) {
                /* KEY_SOFTKEY1 = -6 */
                clazz->static_fields[0].name = strdup("KEY_SOFTKEY1");
                clazz->static_fields[0].descriptor = strdup("I");
                memset(&clazz->static_fields[0].value, 0, sizeof(JavaValue));
                clazz->static_fields[0].value.i = -6;
                
                /* KEY_SOFTKEY2 = -7 */
                clazz->static_fields[1].name = strdup("KEY_SOFTKEY2");
                clazz->static_fields[1].descriptor = strdup("I");
                memset(&clazz->static_fields[1].value, 0, sizeof(JavaValue));
                clazz->static_fields[1].value.i = -7;
                
                /* KEY_SOFTKEY3 = -5 */
                clazz->static_fields[2].name = strdup("KEY_SOFTKEY3");
                clazz->static_fields[2].descriptor = strdup("I");
                memset(&clazz->static_fields[2].value, 0, sizeof(JavaValue));
                clazz->static_fields[2].value.i = -5;
                
                /* KEY_UP_ARROW = -1 */
                clazz->static_fields[3].name = strdup("KEY_UP_ARROW");
                clazz->static_fields[3].descriptor = strdup("I");
                memset(&clazz->static_fields[3].value, 0, sizeof(JavaValue));
                clazz->static_fields[3].value.i = -1;
                
                /* KEY_DOWN_ARROW = -2 */
                clazz->static_fields[4].name = strdup("KEY_DOWN_ARROW");
                clazz->static_fields[4].descriptor = strdup("I");
                memset(&clazz->static_fields[4].value, 0, sizeof(JavaValue));
                clazz->static_fields[4].value.i = -2;
                
                /* KEY_LEFT_ARROW = -3 */
                clazz->static_fields[5].name = strdup("KEY_LEFT_ARROW");
                clazz->static_fields[5].descriptor = strdup("I");
                memset(&clazz->static_fields[5].value, 0, sizeof(JavaValue));
                clazz->static_fields[5].value.i = -3;
                
                /* KEY_RIGHT_ARROW = -4 */
                clazz->static_fields[6].name = strdup("KEY_RIGHT_ARROW");
                clazz->static_fields[6].descriptor = strdup("I");
                memset(&clazz->static_fields[6].value, 0, sizeof(JavaValue));
                clazz->static_fields[6].value.i = -4;
                
                /* KEY_SEND = -10 */
                clazz->static_fields[7].name = strdup("KEY_SEND");
                clazz->static_fields[7].descriptor = strdup("I");
                memset(&clazz->static_fields[7].value, 0, sizeof(JavaValue));
                clazz->static_fields[7].value.i = -10;
                
                /* KEY_END = -11 */
                clazz->static_fields[8].name = strdup("KEY_END");
                clazz->static_fields[8].descriptor = strdup("I");
                memset(&clazz->static_fields[8].value, 0, sizeof(JavaValue));
                clazz->static_fields[8].value.i = -11;
            }
            CLASS_DEBUG(" Created com/nokia/mid/ui/FullCanvas with key constants\n");
        }
        
        /* javax.microedition.media.PlayerImpl - concrete Player implementation */
        else if (clazz->class_name && strcmp(clazz->class_name, "javax/microedition/media/PlayerImpl") == 0) {
            clazz->fields_count = 2;
            clazz->fields = (JavaField*)calloc(2, sizeof(JavaField));
            if (clazz->fields) {
                /* Field 0: playerId (int) */
                clazz->fields[0].name = strdup("playerId");
                clazz->fields[0].descriptor = strdup("I");
                clazz->fields[0].name_index = add_utf8(clazz, "playerId");
                clazz->fields[0].descriptor_index = add_utf8(clazz, "I");
                clazz->fields[0].access_flags = ACC_PRIVATE;

                /* Field 1: state (int) */
                clazz->fields[1].name = strdup("state");
                clazz->fields[1].descriptor = strdup("I");
                clazz->fields[1].name_index = add_utf8(clazz, "state");
                clazz->fields[1].descriptor_index = add_utf8(clazz, "I");
                clazz->fields[1].access_flags = ACC_PRIVATE;

                clazz->instance_size = sizeof(ObjectHeader) + sizeof(JavaValue) * 2;
            }
            
            /* Add interfaces: Player and Controllable */
            clazz->interfaces_count = 2;
            clazz->interfaces = (uint16_t*)calloc(2, sizeof(uint16_t));
            if (clazz->interfaces) {
                clazz->interfaces[0] = add_class_ref(clazz, "javax/microedition/media/Player");
                clazz->interfaces[1] = add_class_ref(clazz, "javax/microedition/media/Controllable");
                CLASS_DEBUG(" Added interfaces to PlayerImpl: Player, Controllable\n");
            }
            
            CLASS_DEBUG(" Added fields to javax/microedition/media/PlayerImpl\n");
        }

        /* v34.53: the media control Impl stubs need a playerId instance
         * field. getControl()/getControls() store player->id into
         * ctrl_obj->fields[0] (skipped when fields_count==0), and every
         * Volume/MIDI/Tone control native reads args[0]->fields[0].i to
         * find its player — with no field the reads returned garbage, so
         * e.g. VolumeControl.setLevel never found its player and volume
         * adjustments silently did nothing. */
        else if (clazz->class_name &&
                 (strcmp(clazz->class_name, "javax/microedition/media/control/VolumeControlImpl") == 0 ||
                  strcmp(clazz->class_name, "javax/microedition/media/control/MIDIControlImpl") == 0 ||
                  strcmp(clazz->class_name, "javax/microedition/media/control/ToneControlImpl") == 0 ||
                  strcmp(clazz->class_name, "javax/microedition/media/control/VideoControlImpl") == 0) &&
                 clazz->fields_count == 0) {
            clazz->fields_count = 1;
            clazz->fields = (JavaField*)calloc(1, sizeof(JavaField));
            if (clazz->fields) {
                clazz->fields[0].name = strdup("playerId");
                clazz->fields[0].descriptor = strdup("I");
                clazz->fields[0].name_index = add_utf8(clazz, "playerId");
                clazz->fields[0].descriptor_index = add_utf8(clazz, "I");
                clazz->fields[0].access_flags = ACC_PRIVATE;
                clazz->instance_size = sizeof(ObjectHeader) + sizeof(JavaValue);
            }
            CLASS_DEBUG(" Added playerId field to %s\n", clazz->class_name);
        }
        
        /* com.nokia.mid.ui.DirectGraphics needs graphics reference */
        else if (clazz->class_name && strcmp(clazz->class_name, "com/nokia/mid/ui/DirectGraphics") == 0) {
            clazz->fields_count = 2;
            clazz->fields = (JavaField*)calloc(2, sizeof(JavaField));
            if (clazz->fields) {
                clazz->fields[0].name = strdup("graphics");
                clazz->fields[0].descriptor = strdup("Ljavax/microedition/lcdui/Graphics;");
                clazz->fields[0].name_index = add_utf8(clazz, "graphics");
                clazz->fields[0].descriptor_index = add_utf8(clazz, "Ljavax/microedition/lcdui/Graphics;");
                clazz->fields[0].access_flags = ACC_PRIVATE;
                
                clazz->fields[1].name = strdup("alphaComponent");
                clazz->fields[1].descriptor = strdup("I");
                clazz->fields[1].name_index = add_utf8(clazz, "alphaComponent");
                clazz->fields[1].descriptor_index = add_utf8(clazz, "I");
                clazz->fields[1].access_flags = ACC_PRIVATE;
                
                clazz->instance_size = sizeof(ObjectHeader) + sizeof(JavaValue) * 2;
            }
            CLASS_DEBUG(" Added fields to com/nokia/mid/ui/DirectGraphics\n");
        }
        
        /* com.nokia.mid.m3d.Texture - v17: image + format for real texturing */
        else if (clazz->class_name && strcmp(clazz->class_name, "com/nokia/mid/m3d/Texture") == 0) {
            clazz->fields_count = 2;
            clazz->fields = (JavaField*)calloc(2, sizeof(JavaField));
            if (clazz->fields) {
                clazz->fields[0].name = strdup("image");
                clazz->fields[0].descriptor = strdup("Ljavax/microedition/lcdui/Image;");
                clazz->fields[0].name_index = add_utf8(clazz, "image");
                clazz->fields[0].descriptor_index = add_utf8(clazz, "Ljavax/microedition/lcdui/Image;");
                clazz->fields[0].access_flags = ACC_PRIVATE;

                clazz->fields[1].name = strdup("format");
                clazz->fields[1].descriptor = strdup("I");
                clazz->fields[1].name_index = add_utf8(clazz, "format");
                clazz->fields[1].descriptor_index = add_utf8(clazz, "I");
                clazz->fields[1].access_flags = ACC_PRIVATE;

                clazz->instance_size = sizeof(ObjectHeader) + sizeof(JavaValue) * 2;
            }
            CLASS_DEBUG(" Added fields to com/nokia/mid/m3d/Texture\n");
        }

        /* com.nokia.mid.ui.SoftNotification - v17: real implementation state */
        else if (clazz->class_name && strcmp(clazz->class_name, "com/nokia/mid/ui/SoftNotification") == 0) {
            clazz->fields_count = 4;
            clazz->fields = (JavaField*)calloc(4, sizeof(JavaField));
            if (clazz->fields) {
                clazz->fields[0].name = strdup("id");
                clazz->fields[0].descriptor = strdup("I");
                clazz->fields[0].name_index = add_utf8(clazz, "id");
                clazz->fields[0].descriptor_index = add_utf8(clazz, "I");
                clazz->fields[0].access_flags = ACC_PRIVATE;

                clazz->fields[1].name = strdup("text");
                clazz->fields[1].descriptor = strdup("Ljava/lang/String;");
                clazz->fields[1].name_index = add_utf8(clazz, "text");
                clazz->fields[1].descriptor_index = add_utf8(clazz, "Ljava/lang/String;");
                clazz->fields[1].access_flags = ACC_PRIVATE;

                clazz->fields[2].name = strdup("image");
                clazz->fields[2].descriptor = strdup("Ljavax/microedition/lcdui/Image;");
                clazz->fields[2].name_index = add_utf8(clazz, "image");
                clazz->fields[2].descriptor_index = add_utf8(clazz, "Ljavax/microedition/lcdui/Image;");
                clazz->fields[2].access_flags = ACC_PRIVATE;

                clazz->fields[3].name = strdup("listener");
                clazz->fields[3].descriptor = strdup("Lcom/nokia/mid/ui/SoftNotificationListener;");
                clazz->fields[3].name_index = add_utf8(clazz, "listener");
                clazz->fields[3].descriptor_index = add_utf8(clazz, "Lcom/nokia/mid/ui/SoftNotificationListener;");
                clazz->fields[3].access_flags = ACC_PRIVATE;

                clazz->instance_size = sizeof(ObjectHeader) + sizeof(JavaValue) * 4;
            }
            CLASS_DEBUG(" Added fields to com/nokia/mid/ui/SoftNotification\n");
        }
        
        /* com.nokia.mid.ui.DirectUtils - static utility class, no instance fields needed */
        else if (clazz->class_name && strcmp(clazz->class_name, "com/nokia/mid/ui/DirectUtils") == 0) {
            clazz->fields_count = 0;
            clazz->instance_size = sizeof(ObjectHeader);
            CLASS_DEBUG(" Created com/nokia/mid/ui/DirectUtils (static utility class)\n");
        }
        
        /* java.io.DataInputStream needs in field */
        else if (clazz->class_name && strcmp(clazz->class_name, "java/io/DataInputStream") == 0) {
            clazz->fields_count = 1;
            clazz->fields = (JavaField*)calloc(1, sizeof(JavaField));
            if (clazz->fields) {
                clazz->fields[0].name = strdup("in");
                clazz->fields[0].descriptor = strdup("Ljava/io/InputStream;");
                clazz->fields[0].name_index = add_utf8(clazz, "in");
                clazz->fields[0].descriptor_index = add_utf8(clazz, "Ljava/io/InputStream;");
                clazz->fields[0].access_flags = ACC_PROTECTED;
                
                clazz->instance_size = sizeof(ObjectHeader) + sizeof(JavaValue);
            }
            CLASS_DEBUG(" Added in field to java/io/DataInputStream\n");
        }
        
        /* java.util.Vector needs elementData, elementCount, capacityIncrement fields */
        else if (clazz->class_name && strcmp(clazz->class_name, "java/util/Vector") == 0) {
            clazz->fields_count = 3;
            clazz->fields = (JavaField*)calloc(3, sizeof(JavaField));
            if (clazz->fields) {
                /* Field 0: elementData (Object[]) */
                clazz->fields[0].name = strdup("elementData");
                clazz->fields[0].descriptor = strdup("[Ljava/lang/Object;");
                clazz->fields[0].name_index = add_utf8(clazz, "elementData");
                clazz->fields[0].descriptor_index = add_utf8(clazz, "[Ljava/lang/Object;");
                clazz->fields[0].access_flags = ACC_PROTECTED;
                
                /* Field 1: elementCount (int) */
                clazz->fields[1].name = strdup("elementCount");
                clazz->fields[1].descriptor = strdup("I");
                clazz->fields[1].name_index = add_utf8(clazz, "elementCount");
                clazz->fields[1].descriptor_index = add_utf8(clazz, "I");
                clazz->fields[1].access_flags = ACC_PROTECTED;
                
                /* Field 2: capacityIncrement (int) */
                clazz->fields[2].name = strdup("capacityIncrement");
                clazz->fields[2].descriptor = strdup("I");
                clazz->fields[2].name_index = add_utf8(clazz, "capacityIncrement");
                clazz->fields[2].descriptor_index = add_utf8(clazz, "I");
                clazz->fields[2].access_flags = ACC_PROTECTED;
                
                clazz->instance_size = sizeof(ObjectHeader) + sizeof(JavaValue) * 3;
            }
            CLASS_DEBUG(" Added fields to java/util/Vector\n");
        }
        
        /* java.util.Hashtable needs table, count, threshold, loadFactor fields */
        else if (clazz->class_name && strcmp(clazz->class_name, "java/util/Hashtable") == 0) {
            clazz->fields_count = 4;
            clazz->fields = (JavaField*)calloc(4, sizeof(JavaField));
            if (clazz->fields) {
                /* Field 0: table (Entry[]) */
                clazz->fields[0].name = strdup("table");
                clazz->fields[0].descriptor = strdup("[Ljava/util/Hashtable$Entry;");
                clazz->fields[0].name_index = add_utf8(clazz, "table");
                clazz->fields[0].descriptor_index = add_utf8(clazz, "[Ljava/util/Hashtable$Entry;");
                clazz->fields[0].access_flags = ACC_PRIVATE;
                
                /* Field 1: count (int) */
                clazz->fields[1].name = strdup("count");
                clazz->fields[1].descriptor = strdup("I");
                clazz->fields[1].name_index = add_utf8(clazz, "count");
                clazz->fields[1].descriptor_index = add_utf8(clazz, "I");
                clazz->fields[1].access_flags = ACC_PRIVATE;
                
                /* Field 2: threshold (int) */
                clazz->fields[2].name = strdup("threshold");
                clazz->fields[2].descriptor = strdup("I");
                clazz->fields[2].name_index = add_utf8(clazz, "threshold");
                clazz->fields[2].descriptor_index = add_utf8(clazz, "I");
                clazz->fields[2].access_flags = ACC_PRIVATE;
                
                /* Field 3: loadFactor (float) */
                clazz->fields[3].name = strdup("loadFactor");
                clazz->fields[3].descriptor = strdup("F");
                clazz->fields[3].name_index = add_utf8(clazz, "loadFactor");
                clazz->fields[3].descriptor_index = add_utf8(clazz, "F");
                clazz->fields[3].access_flags = ACC_PRIVATE;
                
                clazz->instance_size = sizeof(ObjectHeader) + sizeof(JavaValue) * 4;
            }
            CLASS_DEBUG(" Added fields to java/util/Hashtable\n");
        }
        
        /* java.util.Date needs time field (long) */
        else if (clazz->class_name && strcmp(clazz->class_name, "java/util/Date") == 0) {
            clazz->fields_count = 1;
            clazz->fields = (JavaField*)calloc(1, sizeof(JavaField));
            if (clazz->fields) {
                clazz->fields[0].name = strdup("time");
                clazz->fields[0].descriptor = strdup("J");  /* long */
                clazz->fields[0].name_index = add_utf8(clazz, "time");
                clazz->fields[0].descriptor_index = add_utf8(clazz, "J");
                clazz->fields[0].access_flags = ACC_PRIVATE;
                
                clazz->instance_size = sizeof(ObjectHeader) + sizeof(JavaValue) * 2;  /* long = 2 slots */
            }
            CLASS_DEBUG(" Added time field to java/util/Date\n");
        }
        
        /* java.util.Calendar needs time and calendar fields */
        else if (clazz->class_name && strcmp(clazz->class_name, "java/util/Calendar") == 0) {
            clazz->fields_count = 8;
            clazz->fields = (JavaField*)calloc(8, sizeof(JavaField));
            if (clazz->fields) {
                const char* field_names[] = {"time", "year", "month", "dayOfMonth", "hourOfDay", "minute", "second", "isSet"};
                const char* field_descs[] = {"J", "I", "I", "I", "I", "I", "I", "Z"};
                
                for (int i = 0; i < 8; i++) {
                    clazz->fields[i].name = strdup(field_names[i]);
                    clazz->fields[i].descriptor = strdup(field_descs[i]);
                    clazz->fields[i].name_index = add_utf8(clazz, field_names[i]);
                    clazz->fields[i].descriptor_index = add_utf8(clazz, field_descs[i]);
                    clazz->fields[i].access_flags = ACC_PROTECTED;
                }
                
                clazz->instance_size = sizeof(ObjectHeader) + sizeof(JavaValue) * 9;  /* long(2) + 7 ints */
            }
            CLASS_DEBUG(" Added fields to java/util/Calendar\n");
        }
        
        /* java.lang.Byte needs value field */
        else if (clazz->class_name && strcmp(clazz->class_name, "java/lang/Byte") == 0 && clazz->fields_count == 0) {
            clazz->fields_count = 1;
            clazz->fields = (JavaField*)calloc(1, sizeof(JavaField));
            if (clazz->fields) {
                clazz->fields[0].name = strdup("value");
                clazz->fields[0].descriptor = strdup("B");
                clazz->fields[0].name_index = add_utf8(clazz, "value");
                clazz->fields[0].descriptor_index = add_utf8(clazz, "B");
                clazz->fields[0].access_flags = ACC_PRIVATE;
                
                clazz->instance_size = sizeof(ObjectHeader) + sizeof(JavaValue);
            }
            CLASS_DEBUG(" Added value field to java/lang/Byte\n");
        }
        
        /* java.lang.Short needs value field */
        else if (clazz->class_name && strcmp(clazz->class_name, "java/lang/Short") == 0 && clazz->fields_count == 0) {
            clazz->fields_count = 1;
            clazz->fields = (JavaField*)calloc(1, sizeof(JavaField));
            if (clazz->fields) {
                clazz->fields[0].name = strdup("value");
                clazz->fields[0].descriptor = strdup("S");
                clazz->fields[0].name_index = add_utf8(clazz, "value");
                clazz->fields[0].descriptor_index = add_utf8(clazz, "S");
                clazz->fields[0].access_flags = ACC_PRIVATE;
                
                clazz->instance_size = sizeof(ObjectHeader) + sizeof(JavaValue);
            }
            CLASS_DEBUG(" Added value field to java/lang/Short\n");
        }
        
        /* java.lang.Character needs value field */
        else if (clazz->class_name && strcmp(clazz->class_name, "java/lang/Character") == 0 && clazz->fields_count == 0) {
            clazz->fields_count = 1;
            clazz->fields = (JavaField*)calloc(1, sizeof(JavaField));
            if (clazz->fields) {
                clazz->fields[0].name = strdup("value");
                clazz->fields[0].descriptor = strdup("C");
                clazz->fields[0].name_index = add_utf8(clazz, "value");
                clazz->fields[0].descriptor_index = add_utf8(clazz, "C");
                clazz->fields[0].access_flags = ACC_PRIVATE;
                
                clazz->instance_size = sizeof(ObjectHeader) + sizeof(JavaValue);
            }
            CLASS_DEBUG(" Added value field to java/lang/Character\n");
        }
        
        /* ===== M3G (JSR-184) classes: fields are managed by init_m3g_stub_classes() =====
         * in src/midp/mobile3d.c. Adding fields here causes DUPLICATES because
         * init_m3g_stub_classes() also adds the same fields via m3g_add_instance_field().
         * The mobile3d.c version is authoritative — it has the complete field set
         * including *Ref temporary fields, polygonMode, pickingEnable, etc.
         * REMOVED: All M3G field definitions (Object3D through AnimationTrack)
         * that were previously here. Only the class hierarchy setup in
         * get_or_create_stub_class() (super_class assignments) is kept. */
        if (0) { /* disabled M3G field block — see comment above */
        /* Object3D placeholder — now handled by mobile3d.c */
        if (clazz->class_name && strcmp(clazz->class_name, "javax/microedition/m3g/Object3D") == 0 && clazz->fields_count == 0) {
            clazz->fields_count = 3;
            clazz->fields = (JavaField*)calloc(3, sizeof(JavaField));
            if (clazz->fields) {
                clazz->fields[0].name = strdup("userID");
                clazz->fields[0].descriptor = strdup("I");
                clazz->fields[0].name_index = add_utf8(clazz, "userID");
                clazz->fields[0].descriptor_index = add_utf8(clazz, "I");
                clazz->fields[0].access_flags = ACC_PRIVATE;
                
                clazz->fields[1].name = strdup("userObject");
                clazz->fields[1].descriptor = strdup("Ljava/lang/Object;");
                clazz->fields[1].name_index = add_utf8(clazz, "userObject");
                clazz->fields[1].descriptor_index = add_utf8(clazz, "Ljava/lang/Object;");
                clazz->fields[1].access_flags = ACC_PRIVATE;
                
                /* Animation track storage */
                clazz->fields[2].name = strdup("animTracks");
                clazz->fields[2].descriptor = strdup("[Ljavax/microedition/m3g/AnimationTrack;");
                clazz->fields[2].name_index = add_utf8(clazz, "animTracks");
                clazz->fields[2].descriptor_index = add_utf8(clazz, "[Ljavax/microedition/m3g/AnimationTrack;");
                clazz->fields[2].access_flags = ACC_PRIVATE;
                
                clazz->instance_size = sizeof(ObjectHeader) + sizeof(JavaValue) * 3;
            }
            CLASS_DEBUG(" Added fields to javax/microedition/m3g/Object3D\n");
        }
        
        /* javax.microedition.m3g.Transform needs matrix field (float[16]) */
        else if (clazz->class_name && strcmp(clazz->class_name, "javax/microedition/m3g/Transform") == 0 && clazz->fields_count == 0) {
            clazz->fields_count = 1;
            clazz->fields = (JavaField*)calloc(1, sizeof(JavaField));
            if (clazz->fields) {
                clazz->fields[0].name = strdup("matrix");
                clazz->fields[0].descriptor = strdup("[F");
                clazz->fields[0].name_index = add_utf8(clazz, "matrix");
                clazz->fields[0].descriptor_index = add_utf8(clazz, "[F");
                clazz->fields[0].access_flags = ACC_PRIVATE;
                
                clazz->instance_size = sizeof(ObjectHeader) + sizeof(JavaValue);
            }
            CLASS_DEBUG(" Added matrix field to javax/microedition/m3g/Transform\n");
        }
        
        /* javax.microedition.m3g.Transformable needs transform, orientation, scaling fields */
        else if (clazz->class_name && strcmp(clazz->class_name, "javax/microedition/m3g/Transformable") == 0 && clazz->fields_count == 0) {
            clazz->fields_count = 4;
            clazz->fields = (JavaField*)calloc(4, sizeof(JavaField));
            if (clazz->fields) {
                clazz->fields[0].name = strdup("transform");
                clazz->fields[0].descriptor = strdup("[F");
                clazz->fields[0].name_index = add_utf8(clazz, "transform");
                clazz->fields[0].descriptor_index = add_utf8(clazz, "[F");
                clazz->fields[0].access_flags = ACC_PRIVATE;
                
                clazz->fields[1].name = strdup("orientation");
                clazz->fields[1].descriptor = strdup("[F");
                clazz->fields[1].name_index = add_utf8(clazz, "orientation");
                clazz->fields[1].descriptor_index = add_utf8(clazz, "[F");
                clazz->fields[1].access_flags = ACC_PRIVATE;
                
                clazz->fields[2].name = strdup("scaling");
                clazz->fields[2].descriptor = strdup("[F");
                clazz->fields[2].name_index = add_utf8(clazz, "scaling");
                clazz->fields[2].descriptor_index = add_utf8(clazz, "[F");
                clazz->fields[2].access_flags = ACC_PRIVATE;
                
                clazz->fields[3].name = strdup("translation");
                clazz->fields[3].descriptor = strdup("[F");
                clazz->fields[3].name_index = add_utf8(clazz, "translation");
                clazz->fields[3].descriptor_index = add_utf8(clazz, "[F");
                clazz->fields[3].access_flags = ACC_PRIVATE;
                
                /* Transformable extends Object3D (3 fields) + 4 own = 7 total */
                clazz->instance_size = sizeof(ObjectHeader) + sizeof(JavaValue) * 7;
            }
            CLASS_DEBUG(" Added fields to javax/microedition/m3g/Transformable\n");
        }
        
        /* javax.microedition.m3g.Node needs parent, scope, alphaFactor fields */
        else if (clazz->class_name && strcmp(clazz->class_name, "javax/microedition/m3g/Node") == 0 && clazz->fields_count == 0) {
            clazz->fields_count = 16;
            clazz->fields = (JavaField*)calloc(16, sizeof(JavaField));
            if (clazz->fields) {
                clazz->fields[0].name = strdup("parent");
                clazz->fields[0].descriptor = strdup("Ljavax/microedition/m3g/Node;");
                clazz->fields[0].name_index = add_utf8(clazz, "parent");
                clazz->fields[0].descriptor_index = add_utf8(clazz, "Ljavax/microedition/m3g/Node;");
                clazz->fields[0].access_flags = ACC_PRIVATE;
                
                clazz->fields[1].name = strdup("scope");
                clazz->fields[1].descriptor = strdup("I");
                clazz->fields[1].name_index = add_utf8(clazz, "scope");
                clazz->fields[1].descriptor_index = add_utf8(clazz, "I");
                clazz->fields[1].access_flags = ACC_PRIVATE;
                
                clazz->fields[2].name = strdup("alphaFactor");
                clazz->fields[2].descriptor = strdup("F");
                clazz->fields[2].name_index = add_utf8(clazz, "alphaFactor");
                clazz->fields[2].descriptor_index = add_utf8(clazz, "F");
                clazz->fields[2].access_flags = ACC_PRIVATE;
                
                /* M3G transform component fields - used by M3G parser and renderer */
                clazz->fields[3].name = strdup("translationX");
                clazz->fields[3].descriptor = strdup("F");
                clazz->fields[4].name = strdup("translationY");
                clazz->fields[4].descriptor = strdup("F");
                clazz->fields[5].name = strdup("translationZ");
                clazz->fields[5].descriptor = strdup("F");
                clazz->fields[6].name = strdup("scaleX");
                clazz->fields[6].descriptor = strdup("F");
                clazz->fields[7].name = strdup("scaleY");
                clazz->fields[7].descriptor = strdup("F");
                clazz->fields[8].name = strdup("scaleZ");
                clazz->fields[8].descriptor = strdup("F");
                clazz->fields[9].name = strdup("orientationAngle");
                clazz->fields[9].descriptor = strdup("F");
                clazz->fields[10].name = strdup("orientationX");
                clazz->fields[10].descriptor = strdup("F");
                clazz->fields[11].name = strdup("orientationY");
                clazz->fields[11].descriptor = strdup("F");
                clazz->fields[12].name = strdup("orientationZ");
                clazz->fields[12].descriptor = strdup("F");
                /* General 4x4 transform matrix (float[16]) - for M3G files with general transforms */
                clazz->fields[13].name = strdup("transform");
                clazz->fields[13].descriptor = strdup("[F");
                clazz->fields[13].descriptor_index = add_utf8(clazz, "[F");
                /* renderingEnable for M3G nodes */
                clazz->fields[14].name = strdup("renderingEnable");
                clazz->fields[14].descriptor = strdup("I");
                /* userID from Object3D */
                clazz->fields[15].name = strdup("userID");
                clazz->fields[15].descriptor = strdup("I");
                
                /* Node extends Object3D (3 fields) + 16 own = 19 total */
                clazz->instance_size = sizeof(ObjectHeader) + sizeof(JavaValue) * 19;
            }
            CLASS_DEBUG(" Added fields to javax/microedition/m3g/Node\n");
        }
        
        /* javax.microedition.m3g.Group needs children field */
        else if (clazz->class_name && strcmp(clazz->class_name, "javax/microedition/m3g/Group") == 0 && clazz->fields_count == 0) {
            clazz->fields_count = 1;
            clazz->fields = (JavaField*)calloc(1, sizeof(JavaField));
            if (clazz->fields) {
                clazz->fields[0].name = strdup("children");
                clazz->fields[0].descriptor = strdup("[Ljavax/microedition/m3g/Node;");
                clazz->fields[0].name_index = add_utf8(clazz, "children");
                clazz->fields[0].descriptor_index = add_utf8(clazz, "[Ljavax/microedition/m3g/Node;");
                clazz->fields[0].access_flags = ACC_PRIVATE;
                
                /* Group extends Node (19 fields) + 1 own = 20 total */
                clazz->instance_size = sizeof(ObjectHeader) + sizeof(JavaValue) * 20;
            }
            CLASS_DEBUG(" Added fields to javax/microedition/m3g/Group\n");
        }
        
        /* javax.microedition.m3g.World needs activeCamera, background fields */
        else if (clazz->class_name && strcmp(clazz->class_name, "javax/microedition/m3g/World") == 0 && clazz->fields_count == 0) {
            clazz->fields_count = 2;
            clazz->fields = (JavaField*)calloc(2, sizeof(JavaField));
            if (clazz->fields) {
                clazz->fields[0].name = strdup("activeCamera");
                clazz->fields[0].descriptor = strdup("Ljavax/microedition/m3g/Camera;");
                clazz->fields[0].name_index = add_utf8(clazz, "activeCamera");
                clazz->fields[0].descriptor_index = add_utf8(clazz, "Ljavax/microedition/m3g/Camera;");
                clazz->fields[0].access_flags = ACC_PRIVATE;
                
                clazz->fields[1].name = strdup("background");
                clazz->fields[1].descriptor = strdup("Ljavax/microedition/m3g/Background;");
                clazz->fields[1].name_index = add_utf8(clazz, "background");
                clazz->fields[1].descriptor_index = add_utf8(clazz, "Ljavax/microedition/m3g/Background;");
                clazz->fields[1].access_flags = ACC_PRIVATE;
                
                /* World extends Group (20 fields) + 2 own = 22 total */
                clazz->instance_size = sizeof(ObjectHeader) + sizeof(JavaValue) * 22;
            }
            CLASS_DEBUG(" Added fields to javax/microedition/m3g/World\n");
        }
        
        /* javax.microedition.m3g.Camera needs projectionType, fov, near, far fields */
        else if (clazz->class_name && strcmp(clazz->class_name, "javax/microedition/m3g/Camera") == 0 && clazz->fields_count == 0) {
            clazz->fields_count = 6;
            clazz->fields = (JavaField*)calloc(6, sizeof(JavaField));
            if (clazz->fields) {
                clazz->fields[0].name = strdup("projectionType");
                clazz->fields[0].descriptor = strdup("I");
                clazz->fields[0].name_index = add_utf8(clazz, "projectionType");
                clazz->fields[0].descriptor_index = add_utf8(clazz, "I");
                clazz->fields[0].access_flags = ACC_PRIVATE;
                
                clazz->fields[1].name = strdup("fov");
                clazz->fields[1].descriptor = strdup("F");
                clazz->fields[1].name_index = add_utf8(clazz, "fov");
                clazz->fields[1].descriptor_index = add_utf8(clazz, "F");
                clazz->fields[1].access_flags = ACC_PRIVATE;
                
                clazz->fields[2].name = strdup("near");
                clazz->fields[2].descriptor = strdup("F");
                clazz->fields[2].name_index = add_utf8(clazz, "near");
                clazz->fields[2].descriptor_index = add_utf8(clazz, "F");
                clazz->fields[2].access_flags = ACC_PRIVATE;
                
                clazz->fields[3].name = strdup("far");
                clazz->fields[3].descriptor = strdup("F");
                clazz->fields[3].name_index = add_utf8(clazz, "far");
                clazz->fields[3].descriptor_index = add_utf8(clazz, "F");
                clazz->fields[3].access_flags = ACC_PRIVATE;
                
                clazz->fields[4].name = strdup("aspect");
                clazz->fields[4].descriptor = strdup("F");
                /* Generic projection matrix (float[16]) for M3G files */
                clazz->fields[5].name = strdup("genericMatrix");
                clazz->fields[5].descriptor = strdup("[F");
                clazz->fields[5].descriptor_index = add_utf8(clazz, "[F");
                
                /* Camera extends Node (19 fields) + 6 own = 25 total */
                clazz->instance_size = sizeof(ObjectHeader) + sizeof(JavaValue) * 25;
            }
            CLASS_DEBUG(" Added fields to javax/microedition/m3g/Camera\n");
        }
        
        /* javax.microedition.m3g.VertexArray needs data, componentCount, componentSize, vertexCount fields */
        else if (clazz->class_name && strcmp(clazz->class_name, "javax/microedition/m3g/VertexArray") == 0 && clazz->fields_count == 0) {
            clazz->fields_count = 4;
            clazz->fields = (JavaField*)calloc(4, sizeof(JavaField));
            if (clazz->fields) {
                clazz->fields[0].name = strdup("data");
                clazz->fields[0].descriptor = strdup("Ljava/lang/Object;");
                clazz->fields[0].name_index = add_utf8(clazz, "data");
                clazz->fields[0].descriptor_index = add_utf8(clazz, "Ljava/lang/Object;");
                clazz->fields[0].access_flags = ACC_PRIVATE;
                
                clazz->fields[1].name = strdup("componentCount");
                clazz->fields[1].descriptor = strdup("I");
                clazz->fields[1].name_index = add_utf8(clazz, "componentCount");
                clazz->fields[1].descriptor_index = add_utf8(clazz, "I");
                clazz->fields[1].access_flags = ACC_PRIVATE;
                
                clazz->fields[2].name = strdup("componentSize");
                clazz->fields[2].descriptor = strdup("I");
                clazz->fields[2].name_index = add_utf8(clazz, "componentSize");
                clazz->fields[2].descriptor_index = add_utf8(clazz, "I");
                clazz->fields[2].access_flags = ACC_PRIVATE;
                
                clazz->fields[3].name = strdup("vertexCount");
                clazz->fields[3].descriptor = strdup("I");
                clazz->fields[3].name_index = add_utf8(clazz, "vertexCount");
                clazz->fields[3].descriptor_index = add_utf8(clazz, "I");
                clazz->fields[3].access_flags = ACC_PRIVATE;
                
                clazz->instance_size = sizeof(ObjectHeader) + sizeof(JavaValue) * 4;
            }
            CLASS_DEBUG(" Added fields to javax/microedition/m3g/VertexArray\n");
        }
        
        /* javax.microedition.m3g.VertexBuffer needs positions, normals, texCoords, colors, vertexCount fields */
        else if (clazz->class_name && strcmp(clazz->class_name, "javax/microedition/m3g/VertexBuffer") == 0 && clazz->fields_count == 0) {
            clazz->fields_count = 10;
            clazz->fields = (JavaField*)calloc(10, sizeof(JavaField));
            if (clazz->fields) {
                clazz->fields[0].name = strdup("positions");
                clazz->fields[0].descriptor = strdup("Ljavax/microedition/m3g/VertexArray;");
                clazz->fields[0].name_index = add_utf8(clazz, "positions");
                clazz->fields[0].descriptor_index = add_utf8(clazz, "Ljavax/microedition/m3g/VertexArray;");
                clazz->fields[0].access_flags = ACC_PRIVATE;
                
                clazz->fields[1].name = strdup("normals");
                clazz->fields[1].descriptor = strdup("Ljavax/microedition/m3g/VertexArray;");
                clazz->fields[1].name_index = add_utf8(clazz, "normals");
                clazz->fields[1].descriptor_index = add_utf8(clazz, "Ljavax/microedition/m3g/VertexArray;");
                clazz->fields[1].access_flags = ACC_PRIVATE;
                
                clazz->fields[2].name = strdup("texCoords");
                clazz->fields[2].descriptor = strdup("Ljavax/microedition/m3g/VertexArray;");
                clazz->fields[2].name_index = add_utf8(clazz, "texCoords");
                clazz->fields[2].descriptor_index = add_utf8(clazz, "Ljavax/microedition/m3g/VertexArray;");
                clazz->fields[2].access_flags = ACC_PRIVATE;
                
                clazz->fields[3].name = strdup("colors");
                clazz->fields[3].descriptor = strdup("Ljavax/microedition/m3g/VertexArray;");
                clazz->fields[3].name_index = add_utf8(clazz, "colors");
                clazz->fields[3].descriptor_index = add_utf8(clazz, "Ljavax/microedition/m3g/VertexArray;");
                clazz->fields[3].access_flags = ACC_PRIVATE;
                
                clazz->fields[4].name = strdup("vertexCount");
                clazz->fields[4].descriptor = strdup("I");
                clazz->fields[4].name_index = add_utf8(clazz, "vertexCount");
                clazz->fields[4].descriptor_index = add_utf8(clazz, "I");
                clazz->fields[4].access_flags = ACC_PRIVATE;
                
                /* M3G vertex buffer scaling and bias */
                clazz->fields[5].name = strdup("positionScale");
                clazz->fields[5].descriptor = strdup("F");
                clazz->fields[6].name = strdup("biasX");
                clazz->fields[6].descriptor = strdup("F");
                clazz->fields[7].name = strdup("biasY");
                clazz->fields[7].descriptor = strdup("F");
                clazz->fields[8].name = strdup("biasZ");
                clazz->fields[8].descriptor = strdup("F");
                clazz->fields[9].name = strdup("texCoordScale");
                clazz->fields[9].descriptor = strdup("F");
                
                clazz->instance_size = sizeof(ObjectHeader) + sizeof(JavaValue) * 10;
            }
            CLASS_DEBUG(" Added fields to javax/microedition/m3g/VertexBuffer\n");
        }
        
        /* javax.microedition.m3g.IndexBuffer needs indices, indexCount fields */
        else if (clazz->class_name && strcmp(clazz->class_name, "javax/microedition/m3g/IndexBuffer") == 0 && clazz->fields_count == 0) {
            clazz->fields_count = 2;
            clazz->fields = (JavaField*)calloc(2, sizeof(JavaField));
            if (clazz->fields) {
                clazz->fields[0].name = strdup("indices");
                clazz->fields[0].descriptor = strdup("Ljava/lang/Object;");
                clazz->fields[0].name_index = add_utf8(clazz, "indices");
                clazz->fields[0].descriptor_index = add_utf8(clazz, "Ljava/lang/Object;");
                clazz->fields[0].access_flags = ACC_PRIVATE;
                
                clazz->fields[1].name = strdup("indexCount");
                clazz->fields[1].descriptor = strdup("I");
                clazz->fields[1].name_index = add_utf8(clazz, "indexCount");
                clazz->fields[1].descriptor_index = add_utf8(clazz, "I");
                clazz->fields[1].access_flags = ACC_PRIVATE;
                
                clazz->instance_size = sizeof(ObjectHeader) + sizeof(JavaValue) * 2;
            }
            CLASS_DEBUG(" Added fields to javax/microedition/m3g/IndexBuffer\n");
        }
        
        /* javax.microedition.m3g.TriangleStripArray inherits from IndexBuffer */
        else if (clazz->class_name && strcmp(clazz->class_name, "javax/microedition/m3g/TriangleStripArray") == 0 && clazz->fields_count == 0) {
            /* No additional fields beyond IndexBuffer */
            CLASS_DEBUG(" TriangleStripArray uses IndexBuffer fields\n");
        }
        
        /* javax.microedition.m3g.CompositingMode needs blending, depthTest, depthWrite, alphaThreshold fields */
        else if (clazz->class_name && strcmp(clazz->class_name, "javax/microedition/m3g/CompositingMode") == 0 && clazz->fields_count == 0) {
            clazz->fields_count = 4;
            clazz->fields = (JavaField*)calloc(4, sizeof(JavaField));
            if (clazz->fields) {
                clazz->fields[0].name = strdup("blending");
                clazz->fields[0].descriptor = strdup("I");
                clazz->fields[0].access_flags = ACC_PUBLIC;
                
                clazz->fields[1].name = strdup("depthTest");
                clazz->fields[1].descriptor = strdup("Z");
                clazz->fields[1].access_flags = ACC_PUBLIC;
                
                clazz->fields[2].name = strdup("depthWrite");
                clazz->fields[2].descriptor = strdup("Z");
                clazz->fields[2].access_flags = ACC_PUBLIC;
                
                clazz->fields[3].name = strdup("alphaThreshold");
                clazz->fields[3].descriptor = strdup("F");
                clazz->fields[3].access_flags = ACC_PUBLIC;
                
                /* CompositingMode extends Object3D (3 fields) + 4 own = 7 total */
                clazz->instance_size = sizeof(ObjectHeader) + sizeof(JavaValue) * 7;
            }
            CLASS_DEBUG(" Added fields to javax/microedition/m3g/CompositingMode\n");
        }
        
        /* javax.microedition.m3g.Fog needs color, mode, density, nearDistance, farDistance, linear fields */
        else if (clazz->class_name && strcmp(clazz->class_name, "javax/microedition/m3g/Fog") == 0 && clazz->fields_count == 0) {
            clazz->fields_count = 6;
            clazz->fields = (JavaField*)calloc(6, sizeof(JavaField));
            if (clazz->fields) {
                clazz->fields[0].name = strdup("color");
                clazz->fields[0].descriptor = strdup("I");
                clazz->fields[0].access_flags = ACC_PUBLIC;
                
                clazz->fields[1].name = strdup("mode");
                clazz->fields[1].descriptor = strdup("I");
                clazz->fields[1].access_flags = ACC_PUBLIC;
                
                clazz->fields[2].name = strdup("density");
                clazz->fields[2].descriptor = strdup("F");
                clazz->fields[2].access_flags = ACC_PUBLIC;
                
                clazz->fields[3].name = strdup("nearDistance");
                clazz->fields[3].descriptor = strdup("F");
                clazz->fields[3].access_flags = ACC_PUBLIC;
                
                clazz->fields[4].name = strdup("farDistance");
                clazz->fields[4].descriptor = strdup("F");
                clazz->fields[4].access_flags = ACC_PUBLIC;
                
                clazz->fields[5].name = strdup("linear");
                clazz->fields[5].descriptor = strdup("F");
                clazz->fields[5].access_flags = ACC_PUBLIC;
                
                /* Fog extends Object3D (3 fields) + 6 own = 9 total */
                clazz->instance_size = sizeof(ObjectHeader) + sizeof(JavaValue) * 9;
            }
            CLASS_DEBUG(" Added fields to javax/microedition/m3g/Fog\n");
        }
        
        /* javax.microedition.m3g.Mesh needs vertexBuffer, indexBuffer, appearance fields */
        else if (clazz->class_name && strcmp(clazz->class_name, "javax/microedition/m3g/Mesh") == 0 && clazz->fields_count == 0) {
            clazz->fields_count = 3;
            clazz->fields = (JavaField*)calloc(3, sizeof(JavaField));
            if (clazz->fields) {
                clazz->fields[0].name = strdup("vertexBuffer");
                clazz->fields[0].descriptor = strdup("Ljavax/microedition/m3g/VertexBuffer;");
                clazz->fields[0].name_index = add_utf8(clazz, "vertexBuffer");
                clazz->fields[0].descriptor_index = add_utf8(clazz, "Ljavax/microedition/m3g/VertexBuffer;");
                clazz->fields[0].access_flags = ACC_PRIVATE;
                
                clazz->fields[1].name = strdup("indexBuffer");
                clazz->fields[1].descriptor = strdup("Ljavax/microedition/m3g/IndexBuffer;");
                clazz->fields[1].name_index = add_utf8(clazz, "indexBuffer");
                clazz->fields[1].descriptor_index = add_utf8(clazz, "Ljavax/microedition/m3g/IndexBuffer;");
                clazz->fields[1].access_flags = ACC_PRIVATE;
                
                clazz->fields[2].name = strdup("appearance");
                clazz->fields[2].descriptor = strdup("Ljavax/microedition/m3g/Appearance;");
                clazz->fields[2].name_index = add_utf8(clazz, "appearance");
                clazz->fields[2].descriptor_index = add_utf8(clazz, "Ljavax/microedition/m3g/Appearance;");
                clazz->fields[2].access_flags = ACC_PRIVATE;
                
                /* Mesh extends Node (19 fields) + 3 own = 22 total */
                clazz->instance_size = sizeof(ObjectHeader) + sizeof(JavaValue) * 22;
            }
            CLASS_DEBUG(" Added fields to javax/microedition/m3g/Mesh\n");
        }
        
        /* javax.microedition.m3g.Sprite3D needs image, cropX, cropY, cropWidth, cropHeight fields */
        else if (clazz->class_name && strcmp(clazz->class_name, "javax/microedition/m3g/Sprite3D") == 0 && clazz->fields_count == 0) {
            clazz->fields_count = 5;
            clazz->fields = (JavaField*)calloc(5, sizeof(JavaField));
            if (clazz->fields) {
                clazz->fields[0].name = strdup("image");
                clazz->fields[0].descriptor = strdup("Ljavax/microedition/m3g/Image2D;");
                clazz->fields[0].descriptor_index = add_utf8(clazz, "Ljavax/microedition/m3g/Image2D;");
                clazz->fields[0].access_flags = ACC_PUBLIC;
                
                clazz->fields[1].name = strdup("cropX");
                clazz->fields[1].descriptor = strdup("I");
                clazz->fields[1].access_flags = ACC_PUBLIC;
                
                clazz->fields[2].name = strdup("cropY");
                clazz->fields[2].descriptor = strdup("I");
                clazz->fields[2].access_flags = ACC_PUBLIC;
                
                clazz->fields[3].name = strdup("cropWidth");
                clazz->fields[3].descriptor = strdup("I");
                clazz->fields[3].access_flags = ACC_PUBLIC;
                
                clazz->fields[4].name = strdup("cropHeight");
                clazz->fields[4].descriptor = strdup("I");
                clazz->fields[4].access_flags = ACC_PUBLIC;
                
                /* Sprite3D extends Node (19 fields) + 5 own = 24 total */
                clazz->instance_size = sizeof(ObjectHeader) + sizeof(JavaValue) * 24;
            }
            CLASS_DEBUG(" Added fields to javax/microedition/m3g/Sprite3D\n");
        }
        
        /* javax.microedition.m3g.Appearance needs layer, material, texture fields */
        else if (clazz->class_name && strcmp(clazz->class_name, "javax/microedition/m3g/Appearance") == 0 && clazz->fields_count == 0) {
            clazz->fields_count = 5;
            clazz->fields = (JavaField*)calloc(5, sizeof(JavaField));
            if (clazz->fields) {
                clazz->fields[0].name = strdup("layer");
                clazz->fields[0].descriptor = strdup("I");
                clazz->fields[0].name_index = add_utf8(clazz, "layer");
                clazz->fields[0].descriptor_index = add_utf8(clazz, "I");
                clazz->fields[0].access_flags = ACC_PRIVATE;
                
                clazz->fields[1].name = strdup("material");
                clazz->fields[1].descriptor = strdup("Ljavax/microedition/m3g/Material;");
                clazz->fields[1].name_index = add_utf8(clazz, "material");
                clazz->fields[1].descriptor_index = add_utf8(clazz, "Ljavax/microedition/m3g/Material;");
                clazz->fields[1].access_flags = ACC_PRIVATE;
                
                clazz->fields[2].name = strdup("texture");
                clazz->fields[2].descriptor = strdup("Ljavax/microedition/m3g/Texture2D;");
                clazz->fields[2].name_index = add_utf8(clazz, "texture");
                clazz->fields[2].descriptor_index = add_utf8(clazz, "Ljavax/microedition/m3g/Texture2D;");
                clazz->fields[2].access_flags = ACC_PRIVATE;
                
                clazz->fields[3].name = strdup("compositingMode");
                clazz->fields[3].descriptor = strdup("Ljavax/microedition/m3g/CompositingMode;");
                clazz->fields[3].descriptor_index = add_utf8(clazz, "Ljavax/microedition/m3g/CompositingMode;");
                
                clazz->fields[4].name = strdup("fog");
                clazz->fields[4].descriptor = strdup("Ljavax/microedition/m3g/Fog;");
                clazz->fields[4].descriptor_index = add_utf8(clazz, "Ljavax/microedition/m3g/Fog;");
                
                /* Appearance extends Object3D (3 fields) + 5 own = 8 total */
                clazz->instance_size = sizeof(ObjectHeader) + sizeof(JavaValue) * 8;
            }
            CLASS_DEBUG(" Added fields to javax/microedition/m3g/Appearance\n");
        }
        
        /* javax.microedition.m3g.Light needs lightType, color, intensity fields */
        else if (clazz->class_name && strcmp(clazz->class_name, "javax/microedition/m3g/Light") == 0 && clazz->fields_count == 0) {
            clazz->fields_count = 3;
            clazz->fields = (JavaField*)calloc(3, sizeof(JavaField));
            if (clazz->fields) {
                clazz->fields[0].name = strdup("lightType");
                clazz->fields[0].descriptor = strdup("I");
                clazz->fields[0].name_index = add_utf8(clazz, "lightType");
                clazz->fields[0].descriptor_index = add_utf8(clazz, "I");
                clazz->fields[0].access_flags = ACC_PRIVATE;
                
                clazz->fields[1].name = strdup("color");
                clazz->fields[1].descriptor = strdup("I");
                clazz->fields[1].name_index = add_utf8(clazz, "color");
                clazz->fields[1].descriptor_index = add_utf8(clazz, "I");
                clazz->fields[1].access_flags = ACC_PRIVATE;
                
                clazz->fields[2].name = strdup("intensity");
                clazz->fields[2].descriptor = strdup("F");
                clazz->fields[2].name_index = add_utf8(clazz, "intensity");
                clazz->fields[2].descriptor_index = add_utf8(clazz, "F");
                clazz->fields[2].access_flags = ACC_PRIVATE;
                
                /* Light extends Node (19 fields) + 3 own = 22 total */
                clazz->instance_size = sizeof(ObjectHeader) + sizeof(JavaValue) * 22;
            }
            CLASS_DEBUG(" Added fields to javax/microedition/m3g/Light\n");
        }
        
        /* javax.microedition.m3g.Background needs clearColor, backgroundImage fields */
        else if (clazz->class_name && strcmp(clazz->class_name, "javax/microedition/m3g/Background") == 0 && clazz->fields_count == 0) {
            clazz->fields_count = 2;
            clazz->fields = (JavaField*)calloc(2, sizeof(JavaField));
            if (clazz->fields) {
                clazz->fields[0].name = strdup("clearColor");
                clazz->fields[0].descriptor = strdup("I");
                clazz->fields[0].name_index = add_utf8(clazz, "clearColor");
                clazz->fields[0].descriptor_index = add_utf8(clazz, "I");
                clazz->fields[0].access_flags = ACC_PRIVATE;
                
                clazz->fields[1].name = strdup("backgroundImage");
                clazz->fields[1].descriptor = strdup("Ljavax/microedition/m3g/Image2D;");
                clazz->fields[1].name_index = add_utf8(clazz, "backgroundImage");
                clazz->fields[1].descriptor_index = add_utf8(clazz, "Ljavax/microedition/m3g/Image2D;");
                clazz->fields[1].access_flags = ACC_PRIVATE;
                
                /* Background extends Object3D (3 fields) + 2 own = 5 total */
                clazz->instance_size = sizeof(ObjectHeader) + sizeof(JavaValue) * 5;
            }
            CLASS_DEBUG(" Added fields to javax/microedition/m3g/Background\n");
        }
        
        /* javax.microedition.m3g.Image2D needs width, height, format, pixels fields */
        else if (clazz->class_name && strcmp(clazz->class_name, "javax/microedition/m3g/Image2D") == 0 && clazz->fields_count == 0) {
            clazz->fields_count = 4;
            clazz->fields = (JavaField*)calloc(4, sizeof(JavaField));
            if (clazz->fields) {
                clazz->fields[0].name = strdup("width");
                clazz->fields[0].descriptor = strdup("I");
                clazz->fields[0].name_index = add_utf8(clazz, "width");
                clazz->fields[0].descriptor_index = add_utf8(clazz, "I");
                clazz->fields[0].access_flags = ACC_PRIVATE;
                
                clazz->fields[1].name = strdup("height");
                clazz->fields[1].descriptor = strdup("I");
                clazz->fields[1].name_index = add_utf8(clazz, "height");
                clazz->fields[1].descriptor_index = add_utf8(clazz, "I");
                clazz->fields[1].access_flags = ACC_PRIVATE;
                
                clazz->fields[2].name = strdup("format");
                clazz->fields[2].descriptor = strdup("I");
                clazz->fields[2].name_index = add_utf8(clazz, "format");
                clazz->fields[2].descriptor_index = add_utf8(clazz, "I");
                clazz->fields[2].access_flags = ACC_PRIVATE;
                
                clazz->fields[3].name = strdup("pixels");
                clazz->fields[3].descriptor = strdup("[I");
                clazz->fields[3].name_index = add_utf8(clazz, "pixels");
                clazz->fields[3].descriptor_index = add_utf8(clazz, "[I");
                clazz->fields[3].access_flags = ACC_PRIVATE;
                
                /* Image2D extends Object3D (3 fields) + 4 own = 7 total */
                clazz->instance_size = sizeof(ObjectHeader) + sizeof(JavaValue) * 7;
            }
            CLASS_DEBUG(" Added fields to javax/microedition/m3g/Image2D\n");
        }
        
        /* javax.microedition.m3g.Material needs ambient, diffuse, specular, emissive, shininess fields */
        else if (clazz->class_name && strcmp(clazz->class_name, "javax/microedition/m3g/Material") == 0 && clazz->fields_count == 0) {
            clazz->fields_count = 6;
            clazz->fields = (JavaField*)calloc(6, sizeof(JavaField));
            if (clazz->fields) {
                clazz->fields[0].name = strdup("ambient");
                clazz->fields[0].descriptor = strdup("I");
                clazz->fields[0].name_index = add_utf8(clazz, "ambient");
                clazz->fields[0].descriptor_index = add_utf8(clazz, "I");
                clazz->fields[0].access_flags = ACC_PRIVATE;
                
                clazz->fields[1].name = strdup("diffuse");
                clazz->fields[1].descriptor = strdup("I");
                clazz->fields[1].name_index = add_utf8(clazz, "diffuse");
                clazz->fields[1].descriptor_index = add_utf8(clazz, "I");
                clazz->fields[1].access_flags = ACC_PRIVATE;
                
                clazz->fields[2].name = strdup("specular");
                clazz->fields[2].descriptor = strdup("I");
                clazz->fields[2].name_index = add_utf8(clazz, "specular");
                clazz->fields[2].descriptor_index = add_utf8(clazz, "I");
                clazz->fields[2].access_flags = ACC_PRIVATE;
                
                clazz->fields[3].name = strdup("emissive");
                clazz->fields[3].descriptor = strdup("I");
                clazz->fields[3].name_index = add_utf8(clazz, "emissive");
                clazz->fields[3].descriptor_index = add_utf8(clazz, "I");
                clazz->fields[3].access_flags = ACC_PRIVATE;
                
                clazz->fields[4].name = strdup("shininess");
                clazz->fields[4].descriptor = strdup("F");
                clazz->fields[4].name_index = add_utf8(clazz, "shininess");
                clazz->fields[4].descriptor_index = add_utf8(clazz, "F");
                clazz->fields[4].access_flags = ACC_PRIVATE;
                
                clazz->fields[5].name = strdup("color");
                clazz->fields[5].descriptor = strdup("I");
                clazz->fields[5].access_flags = ACC_PUBLIC;
                
                /* Material extends Object3D (3 fields) + 6 own = 9 total */
                clazz->instance_size = sizeof(ObjectHeader) + sizeof(JavaValue) * 9;
            }
            CLASS_DEBUG(" Added fields to javax/microedition/m3g/Material\n");
        }
        
        /* javax.microedition.m3g.Texture2D needs image, blendColor, filtering fields */
        else if (clazz->class_name && strcmp(clazz->class_name, "javax/microedition/m3g/Texture2D") == 0 && clazz->fields_count == 0) {
            clazz->fields_count = 3;
            clazz->fields = (JavaField*)calloc(3, sizeof(JavaField));
            if (clazz->fields) {
                clazz->fields[0].name = strdup("image");
                clazz->fields[0].descriptor = strdup("Ljavax/microedition/m3g/Image2D;");
                clazz->fields[0].name_index = add_utf8(clazz, "image");
                clazz->fields[0].descriptor_index = add_utf8(clazz, "Ljavax/microedition/m3g/Image2D;");
                clazz->fields[0].access_flags = ACC_PRIVATE;
                
                clazz->fields[1].name = strdup("blendColor");
                clazz->fields[1].descriptor = strdup("I");
                clazz->fields[1].name_index = add_utf8(clazz, "blendColor");
                clazz->fields[1].descriptor_index = add_utf8(clazz, "I");
                clazz->fields[1].access_flags = ACC_PRIVATE;
                
                clazz->fields[2].name = strdup("filtering");
                clazz->fields[2].descriptor = strdup("I");
                clazz->fields[2].name_index = add_utf8(clazz, "filtering");
                clazz->fields[2].descriptor_index = add_utf8(clazz, "I");
                clazz->fields[2].access_flags = ACC_PRIVATE;
                
                /* Texture2D extends Object3D (3 fields) + 3 own = 6 total */
                clazz->instance_size = sizeof(ObjectHeader) + sizeof(JavaValue) * 6;
            }
            CLASS_DEBUG(" Added fields to javax/microedition/m3g/Texture2D\n");
        }

        /* javax.microedition.m3g.KeyframeSequence needs keyframe data fields */
        else if (clazz->class_name && strcmp(clazz->class_name, "javax/microedition/m3g/KeyframeSequence") == 0 && clazz->fields_count == 0) {
            /* Fields: numKeyframes(I), numComponents(I), interpolation(I),
               duration(I), repeatMode(I), firstValid(I), lastValid(I),
               keyframeTimes([I), keyframeValues([F) */
            clazz->fields_count = 9;
            clazz->fields = (JavaField*)calloc(9, sizeof(JavaField));
            if (clazz->fields) {
                int fi = 0;
                /* Field 0: numKeyframes */
                clazz->fields[fi].name = strdup("numKeyframes");
                clazz->fields[fi].descriptor = strdup("I");
                clazz->fields[fi].name_index = add_utf8(clazz, "numKeyframes");
                clazz->fields[fi].descriptor_index = add_utf8(clazz, "I");
                clazz->fields[fi].access_flags = ACC_PRIVATE;
                fi++;
                /* Field 1: numComponents */
                clazz->fields[fi].name = strdup("numComponents");
                clazz->fields[fi].descriptor = strdup("I");
                clazz->fields[fi].name_index = add_utf8(clazz, "numComponents");
                clazz->fields[fi].descriptor_index = add_utf8(clazz, "I");
                clazz->fields[fi].access_flags = ACC_PRIVATE;
                fi++;
                /* Field 2: interpolation */
                clazz->fields[fi].name = strdup("interpolation");
                clazz->fields[fi].descriptor = strdup("I");
                clazz->fields[fi].name_index = add_utf8(clazz, "interpolation");
                clazz->fields[fi].descriptor_index = add_utf8(clazz, "I");
                clazz->fields[fi].access_flags = ACC_PRIVATE;
                fi++;
                /* Field 3: duration */
                clazz->fields[fi].name = strdup("duration");
                clazz->fields[fi].descriptor = strdup("I");
                clazz->fields[fi].name_index = add_utf8(clazz, "duration");
                clazz->fields[fi].descriptor_index = add_utf8(clazz, "I");
                clazz->fields[fi].access_flags = ACC_PRIVATE;
                fi++;
                /* Field 4: repeatMode (CONSTANT=192, LOOP=193) */
                clazz->fields[fi].name = strdup("repeatMode");
                clazz->fields[fi].descriptor = strdup("I");
                clazz->fields[fi].name_index = add_utf8(clazz, "repeatMode");
                clazz->fields[fi].descriptor_index = add_utf8(clazz, "I");
                clazz->fields[fi].access_flags = ACC_PRIVATE;
                fi++;
                /* Field 5: firstValid */
                clazz->fields[fi].name = strdup("firstValid");
                clazz->fields[fi].descriptor = strdup("I");
                clazz->fields[fi].name_index = add_utf8(clazz, "firstValid");
                clazz->fields[fi].descriptor_index = add_utf8(clazz, "I");
                clazz->fields[fi].access_flags = ACC_PRIVATE;
                fi++;
                /* Field 6: lastValid */
                clazz->fields[fi].name = strdup("lastValid");
                clazz->fields[fi].descriptor = strdup("I");
                clazz->fields[fi].name_index = add_utf8(clazz, "lastValid");
                clazz->fields[fi].descriptor_index = add_utf8(clazz, "I");
                clazz->fields[fi].access_flags = ACC_PRIVATE;
                fi++;
                /* Field 7: keyframeTimes (int[]) */
                clazz->fields[fi].name = strdup("keyframeTimes");
                clazz->fields[fi].descriptor = strdup("[I");
                clazz->fields[fi].name_index = add_utf8(clazz, "keyframeTimes");
                clazz->fields[fi].descriptor_index = add_utf8(clazz, "[I");
                clazz->fields[fi].access_flags = ACC_PRIVATE;
                fi++;
                /* Field 8: keyframeValues (float[] - flat array of all keyframe values) */
                clazz->fields[fi].name = strdup("keyframeValues");
                clazz->fields[fi].descriptor = strdup("[F");
                clazz->fields[fi].name_index = add_utf8(clazz, "keyframeValues");
                clazz->fields[fi].descriptor_index = add_utf8(clazz, "[F");
                clazz->fields[fi].access_flags = ACC_PRIVATE;

                /* KeyframeSequence extends Object3D (3 fields) + 9 own = 12 total */
                clazz->instance_size = sizeof(ObjectHeader) + sizeof(JavaValue) * 12;
            }
            CLASS_DEBUG(" Added fields to javax/microedition/m3g/KeyframeSequence\n");
        }

        /* javax.microedition.m3g.AnimationController needs timing/weight fields */
        else if (clazz->class_name && strcmp(clazz->class_name, "javax/microedition/m3g/AnimationController") == 0 && clazz->fields_count == 0) {
            /* Fields: activationTime(I), deactivationTime(I), weight(F),
               speed(F), refWorldTime(I), refSequenceTime(F) */
            clazz->fields_count = 6;
            clazz->fields = (JavaField*)calloc(6, sizeof(JavaField));
            if (clazz->fields) {
                int fi = 0;
                /* Field 0: activationTime */
                clazz->fields[fi].name = strdup("activationTime");
                clazz->fields[fi].descriptor = strdup("I");
                clazz->fields[fi].name_index = add_utf8(clazz, "activationTime");
                clazz->fields[fi].descriptor_index = add_utf8(clazz, "I");
                clazz->fields[fi].access_flags = ACC_PRIVATE;
                fi++;
                /* Field 1: deactivationTime */
                clazz->fields[fi].name = strdup("deactivationTime");
                clazz->fields[fi].descriptor = strdup("I");
                clazz->fields[fi].name_index = add_utf8(clazz, "deactivationTime");
                clazz->fields[fi].descriptor_index = add_utf8(clazz, "I");
                clazz->fields[fi].access_flags = ACC_PRIVATE;
                fi++;
                /* Field 2: weight */
                clazz->fields[fi].name = strdup("weight");
                clazz->fields[fi].descriptor = strdup("F");
                clazz->fields[fi].name_index = add_utf8(clazz, "weight");
                clazz->fields[fi].descriptor_index = add_utf8(clazz, "F");
                clazz->fields[fi].access_flags = ACC_PRIVATE;
                fi++;
                /* Field 3: speed */
                clazz->fields[fi].name = strdup("speed");
                clazz->fields[fi].descriptor = strdup("F");
                clazz->fields[fi].name_index = add_utf8(clazz, "speed");
                clazz->fields[fi].descriptor_index = add_utf8(clazz, "F");
                clazz->fields[fi].access_flags = ACC_PRIVATE;
                fi++;
                /* Field 4: refWorldTime */
                clazz->fields[fi].name = strdup("refWorldTime");
                clazz->fields[fi].descriptor = strdup("I");
                clazz->fields[fi].name_index = add_utf8(clazz, "refWorldTime");
                clazz->fields[fi].descriptor_index = add_utf8(clazz, "I");
                clazz->fields[fi].access_flags = ACC_PRIVATE;
                fi++;
                /* Field 5: refSequenceTime */
                clazz->fields[fi].name = strdup("refSequenceTime");
                clazz->fields[fi].descriptor = strdup("F");
                clazz->fields[fi].name_index = add_utf8(clazz, "refSequenceTime");
                clazz->fields[fi].descriptor_index = add_utf8(clazz, "F");
                clazz->fields[fi].access_flags = ACC_PRIVATE;

                /* AnimationController extends Object3D (3 fields) + 6 own = 9 total */
                clazz->instance_size = sizeof(ObjectHeader) + sizeof(JavaValue) * 9;
            }
            CLASS_DEBUG(" Added fields to javax/microedition/m3g/AnimationController\n");
        }

        /* javax.microedition.m3g.AnimationTrack needs sequence, controller, propertyId */
        else if (clazz->class_name && strcmp(clazz->class_name, "javax/microedition/m3g/AnimationTrack") == 0 && clazz->fields_count == 0) {
            clazz->fields_count = 3;
            clazz->fields = (JavaField*)calloc(3, sizeof(JavaField));
            if (clazz->fields) {
                /* Field 0: sequence (KeyframeSequence ref) */
                clazz->fields[0].name = strdup("sequence");
                clazz->fields[0].descriptor = strdup("Ljavax/microedition/m3g/KeyframeSequence;");
                clazz->fields[0].name_index = add_utf8(clazz, "sequence");
                clazz->fields[0].descriptor_index = add_utf8(clazz, "Ljavax/microedition/m3g/KeyframeSequence;");
                clazz->fields[0].access_flags = ACC_PRIVATE;
                
                /* Field 1: controller (AnimationController ref) */
                clazz->fields[1].name = strdup("controller");
                clazz->fields[1].descriptor = strdup("Ljavax/microedition/m3g/AnimationController;");
                clazz->fields[1].name_index = add_utf8(clazz, "controller");
                clazz->fields[1].descriptor_index = add_utf8(clazz, "Ljavax/microedition/m3g/AnimationController;");
                clazz->fields[1].access_flags = ACC_PRIVATE;
                
                /* Field 2: propertyId */
                clazz->fields[2].name = strdup("propertyId");
                clazz->fields[2].descriptor = strdup("I");
                clazz->fields[2].name_index = add_utf8(clazz, "propertyId");
                clazz->fields[2].descriptor_index = add_utf8(clazz, "I");
                clazz->fields[2].access_flags = ACC_PRIVATE;
                
                /* AnimationTrack extends Object3D (3 fields) + 3 own = 6 total */
                clazz->instance_size = sizeof(ObjectHeader) + sizeof(JavaValue) * 6;
            }
            CLASS_DEBUG(" Added fields to javax/microedition/m3g/AnimationTrack\n");
        }
        } /* end if(0) — disabled M3G field block */
    }

    /* ===== ЧЕТВЕРТЫЙ ПРОХОД: Правильный расчет instance_size с учетом наследования ===== */
    /*
     * CRITICAL FIX: instance_size должен включать все поля из всей иерархии классов.
     * Используем jvm_recalculate_instance_size() из execute.c - она рекурсивно
     * обрабатывает суперклассы, гарантируя правильный порядок вычисления.
     * Предыдущая реализация имела баг: если суперкласс шел ПОСЛЕ дочернего
     * в списке class_loader, его instance_size еще не был обновлен.
     */
    for (size_t i = 0; i < jvm->class_loader.count; i++) {
        JavaClass* clazz = jvm->class_loader.classes[i];

        /* SKIP M3G (JSR-184) classes — their instance_size and fields are
         * managed exclusively by init_m3g_stub_classes() in mobile3d.c.
         * Running jvm_recalculate_instance_size() here on M3G stubs (which
         * have fields_count=0 at this point) would set instance_size to just
         * sizeof(ObjectHeader). Later, init_m3g_stub_classes() adds fields
         * and recalculates correctly, BUT any subsequent jvm_init_class()
         * call would re-invoke jvm_recalculate_instance_size() which could
         * produce a DIFFERENT size if the class hierarchy differs, causing
         * objects allocated with one size to be accessed with another —
         * classic heap corruption. */
        if (clazz->class_name && strstr(clazz->class_name, "javax/microedition/m3g/") != NULL) {
            continue;
        }

        size_t old_size = clazz->instance_size;
        jvm_recalculate_instance_size(jvm, clazz);
        if (clazz->instance_size != old_size) {
            CLASS_DEBUG(" Fixed instance_size for %s: %zu -> %zu\n",
                       clazz->class_name ? clazz->class_name : "?",
                       old_size, (size_t)clazz->instance_size);
        }
    }

    /* ===== ПЯТЫЙ ПРОХОД: Устанавливаем ObjectHeader для всех классов ===== */
    JavaClass* class_class = NULL;  /* java/lang/Class */
    
    /* Находим java/lang/Class */
    for (size_t i = 0; i < jvm->class_loader.count; i++) {
        JavaClass* clazz = jvm->class_loader.classes[i];
        if (clazz->class_name && strcmp(clazz->class_name, "java/lang/Class") == 0) {
            class_class = clazz;
            break;
        }
    }

    /* v20: materialize Boolean.TRUE/FALSE canonical instances NOW — all
     * wrapper stubs have their instance fields (incl. Boolean.value) and
     * final instance_size at this point. */
    {
        JavaClass* bool_cls = NULL;
        for (size_t i = 0; i < jvm->class_loader.count; i++) {
            JavaClass* c = jvm->class_loader.classes[i];
            if (c->class_name && strcmp(c->class_name, "java/lang/Boolean") == 0) { bool_cls = c; break; }
        }
        if (bool_cls) {
            for (int s = 0; s < bool_cls->static_fields_count; s++) {
                if (!bool_cls->static_fields[s].name) continue;
                int is_true  = strcmp(bool_cls->static_fields[s].name, "TRUE") == 0;
                int is_false = strcmp(bool_cls->static_fields[s].name, "FALSE") == 0;
                if (!is_true && !is_false) continue;
                JavaObject* b = jvm_new_object(jvm, bool_cls);
                if (b) {
                    JavaValue v; memset(&v, 0, sizeof(v));
                    v.i = is_true ? 1 : 0;
                    native_set_field_value(b, "value", v);
                }
                bool_cls->static_fields[s].value.ref = b;
                if (b) jvm_add_root(jvm, b);  /* keep canonical TRUE/FALSE alive */
            }
        }
    }

    if (class_class) {
        for (size_t i = 0; i < jvm->class_loader.count; i++) {
            JavaClass* clazz = jvm->class_loader.classes[i];
            /* Set header.clazz to java/lang/Class so this JavaClass* can be used as a Class object */
            clazz->header.clazz = class_class;
            /* Set a hashcode based on the pointer */
            clazz->header.hashcode = (jint)(uintptr_t)clazz ^ 0x5A5A5A5A;
            clazz->header.gc_mark = 0;
            clazz->header.reserved = 0;
        }
        /* java/lang/Class's header.clazz should point to itself */
        class_class->header.clazz = class_class;
        CLASS_DEBUG(" Initialized Class object headers for %zu classes", jvm->class_loader.count);
    }

    return count;
}

/* Get or create a stub class by name */
JavaClass* get_or_create_stub_class(JVM* jvm, const char* class_name) {
    if (!class_name || !jvm) return NULL;
    
    /* Check if already loaded (linear search + hash table) */
    for (size_t i = 0; i < jvm->class_loader.count; i++) {
        if (jvm->class_loader.classes[i]->class_name &&
            strcmp(jvm->class_loader.classes[i]->class_name, class_name) == 0) {
            /* v34.49 FIX (3D Ferrari): classfile_parse() resolves every
             * superclass/interface through HERE, and the early-return below
             * handed out the BUILTIN stub (com/nokia/mid/ui/FullCanvas
             * extends plain Canvas) even when the MIDlet ships its own
             * version (extends com/hellomoto/fullscreen/FullCn). The broken
             * hierarchy then threw ClassCastException in GameMIDlet.<init>
             * (swallowed), skipping the text-resource init — the midlet
             * died with an NPE in Ferrari3D.<init>. Give the JAR the same
             * override chance the hash path has had since v20. */
            JavaClass* found = jvm->class_loader.classes[i];
            if (found->is_stub) {
                JavaClass* real = jvm_stub_jar_override_linear(jvm, class_name, found);
                if (real) return real;
            }
            return found;
        }
    }
    
    /* CRITICAL FIX: Try to load from JAR before creating a stub!
     * Many obfuscated J2ME games have classes with short names (a, b, c...ap, as, etc.)
     * that get stubbed prematurely when they're referenced as superclasses or in
     * field/method signatures during classfile_parse of other classes.
     * If the JAR data is available, we should try to load the real class first. */
    if (jvm->class_loader.jar_data && jvm->class_loader.jar_size > 0) {
        JavaClass* jar_class = jvm_load_class_from_jar(jvm, class_name);
        if (jar_class) {
            fprintf(stderr, "[STUB-FIX] Loaded '%s' from JAR instead of creating stub "
                    "(methods=%d, fields=%d)\n",
                    class_name, (int)jar_class->methods_count, (int)jar_class->fields_count);
            return jar_class;
        }
    }
    
    /* Determine super class based on class name */
    const char* super_name = "java/lang/Object";  /* Default */
    
    /* MIDP LCDUI hierarchy */
    if (strcmp(class_name, "javax/microedition/lcdui/game/GameCanvas") == 0) {
        super_name = "javax/microedition/lcdui/Canvas";
    } else if (strcmp(class_name, "javax/microedition/lcdui/game/Layer") == 0) {
        super_name = "java/lang/Object";
    } else if (strcmp(class_name, "javax/microedition/lcdui/game/Sprite") == 0 ||
               strcmp(class_name, "javax/microedition/lcdui/game/TiledLayer") == 0) {
        super_name = "javax/microedition/lcdui/game/Layer";
    } else if (strcmp(class_name, "javax/microedition/lcdui/game/LayerManager") == 0) {
        super_name = "java/lang/Object";
    } else if (strcmp(class_name, "javax/microedition/lcdui/Canvas") == 0) {
        super_name = "javax/microedition/lcdui/Displayable";
    } else if (strcmp(class_name, "javax/microedition/lcdui/Displayable") == 0) {
        super_name = "java/lang/Object";
    } else if (strcmp(class_name, "javax/microedition/lcdui/Screen") == 0) {
        super_name = "javax/microedition/lcdui/Displayable";
    } else if (strcmp(class_name, "javax/microedition/lcdui/Form") == 0) {
        super_name = "javax/microedition/lcdui/Screen";
    } else if (strcmp(class_name, "javax/microedition/lcdui/TextBox") == 0) {
        super_name = "javax/microedition/lcdui/Screen";
    } else if (strcmp(class_name, "javax/microedition/lcdui/List") == 0) {
        super_name = "javax/microedition/lcdui/Screen";
    } else if (strcmp(class_name, "javax/microedition/lcdui/Alert") == 0) {
        super_name = "javax/microedition/lcdui/Screen";
    } else if (strcmp(class_name, "javax/microedition/lcdui/Item") == 0) {
        super_name = "java/lang/Object";
    } else if (strcmp(class_name, "javax/microedition/lcdui/StringItem") == 0 ||
               strcmp(class_name, "javax/microedition/lcdui/TextField") == 0 ||
               strcmp(class_name, "javax/microedition/lcdui/ImageItem") == 0 ||
               strcmp(class_name, "javax/microedition/lcdui/ChoiceGroup") == 0 ||
               strcmp(class_name, "javax/microedition/lcdui/Gauge") == 0 ||
               strcmp(class_name, "javax/microedition/lcdui/Spacer") == 0 ||
               strcmp(class_name, "javax/microedition/lcdui/DateField") == 0) {
        super_name = "javax/microedition/lcdui/Item";
    }
    /* MIDP Media */
    else if (strcmp(class_name, "javax/microedition/media/Player") == 0) {
        super_name = "java/lang/Object";
    } else if (strcmp(class_name, "javax/microedition/media/Control") == 0) {
        super_name = "java/lang/Object";
    } else if (strcmp(class_name, "javax/microedition/media/control/VolumeControl") == 0) {
        super_name = "javax/microedition/media/Control";
    }
    /* RMS */
    else if (strcmp(class_name, "javax/microedition/rms/RecordStoreException") == 0) {
        super_name = "java/lang/Exception";
    } else if (strcmp(class_name, "javax/microedition/rms/RecordStoreNotFoundException") == 0 ||
               strcmp(class_name, "javax/microedition/rms/RecordStoreFullException") == 0 ||
               strcmp(class_name, "javax/microedition/rms/RecordStoreNotOpenException") == 0 ||
               strcmp(class_name, "javax/microedition/rms/InvalidRecordIDException") == 0) {
        super_name = "javax/microedition/rms/RecordStoreException";
    }
    /* Exceptions */
    else if (strcmp(class_name, "javax/microedition/midlet/MIDletStateChangeException") == 0) {
        super_name = "java/lang/Exception";
    }
    /* I/O Exceptions - EOFException extends IOException */
    else if (strcmp(class_name, "java/io/EOFException") == 0) {
        super_name = "java/io/IOException";
    }
    else if (strcmp(class_name, "java/io/UTFDataFormatException") == 0) {
        super_name = "java/io/IOException";
    }
    else if (strcmp(class_name, "java/io/FileNotFoundException") == 0) {
        super_name = "java/io/IOException";
    }
    else if (strcmp(class_name, "java/io/InterruptedIOException") == 0) {
        super_name = "java/io/IOException";
    }
    /* Security exceptions */
    else if (strcmp(class_name, "java/lang/SecurityException") == 0) {
        super_name = "java/lang/RuntimeException";
    }
    /* Unsupported operations */
    else if (strcmp(class_name, "java/lang/UnsupportedOperationException") == 0) {
        super_name = "java/lang/RuntimeException";
    }
    /* NegativeArraySizeException */
    else if (strcmp(class_name, "java/lang/NegativeArraySizeException") == 0) {
        super_name = "java/lang/RuntimeException";
    }
    /* ArithmeticException */
    else if (strcmp(class_name, "java/lang/ArithmeticException") == 0) {
        super_name = "java/lang/RuntimeException";
    }
    /* IllegalStateException */
    else if (strcmp(class_name, "java/lang/IllegalMonitorStateException") == 0) {
        super_name = "java/lang/RuntimeException";
    }
    /* LinkageError and subclasses */
    else if (strcmp(class_name, "java/lang/LinkageError") == 0) {
        super_name = "java/lang/Error";
    }
    else if (strcmp(class_name, "java/lang/NoSuchMethodError") == 0 ||
             strcmp(class_name, "java/lang/NoSuchFieldError") == 0 ||
             strcmp(class_name, "java/lang/NoClassDefFoundError") == 0) {
        super_name = "java/lang/Error";
    }
    else if (strcmp(class_name, "java/lang/ClassFormatError") == 0 ||
             strcmp(class_name, "java/lang/VerifyError") == 0) {
        super_name = "java/lang/Error";
    }
    
    /* JSR 184 M3G (Mobile 3D Graphics) class hierarchy */
    else if (strncmp(class_name, "javax/microedition/m3g/", 23) == 0) {
        /* Core M3G classes */
        if (strcmp(class_name, "javax/microedition/m3g/Object3D") == 0) {
            super_name = "java/lang/Object";
        } else if (strcmp(class_name, "javax/microedition/m3g/Transform") == 0) {
            super_name = "java/lang/Object";
        } else if (strcmp(class_name, "javax/microedition/m3g/Transformable") == 0) {
            super_name = "javax/microedition/m3g/Object3D";
        } else if (strcmp(class_name, "javax/microedition/m3g/Node") == 0) {
            super_name = "javax/microedition/m3g/Object3D";
        } else if (strcmp(class_name, "javax/microedition/m3g/Group") == 0 ||
                   strcmp(class_name, "javax/microedition/m3g/Camera") == 0 ||
                   strcmp(class_name, "javax/microedition/m3g/Mesh") == 0 ||
                   strcmp(class_name, "javax/microedition/m3g/Sprite3D") == 0 ||
                   strcmp(class_name, "javax/microedition/m3g/Light") == 0) {
            super_name = "javax/microedition/m3g/Node";
        } else if (strcmp(class_name, "javax/microedition/m3g/World") == 0) {
            super_name = "javax/microedition/m3g/Group";
        } else if (strcmp(class_name, "javax/microedition/m3g/IndexBuffer") == 0) {
            /* JSR-184: IndexBuffer extends Object3D (not java/lang/Object) */
            super_name = "javax/microedition/m3g/Object3D";
        } else if (strcmp(class_name, "javax/microedition/m3g/TriangleStripArray") == 0) {
            super_name = "javax/microedition/m3g/IndexBuffer";
        } else if (strcmp(class_name, "javax/microedition/m3g/Image2D") == 0 ||
                   strcmp(class_name, "javax/microedition/m3g/Background") == 0 ||
                   strcmp(class_name, "javax/microedition/m3g/Texture2D") == 0) {
            super_name = "javax/microedition/m3g/Object3D";
        } else if (strcmp(class_name, "javax/microedition/m3g/MorphingMesh") == 0 ||
                   strcmp(class_name, "javax/microedition/m3g/SkinnedMesh") == 0) {
            super_name = "javax/microedition/m3g/Mesh";
        } else if (strcmp(class_name, "javax/microedition/m3g/Appearance") == 0 ||
                   strcmp(class_name, "javax/microedition/m3g/Material") == 0 ||
                   strcmp(class_name, "javax/microedition/m3g/VertexBuffer") == 0 ||
                   strcmp(class_name, "javax/microedition/m3g/VertexArray") == 0) {
            /* JSR-184: Appearance, Material, VertexBuffer, VertexArray all extend Object3D */
            super_name = "javax/microedition/m3g/Object3D";
        } else if (strcmp(class_name, "javax/microedition/m3g/KeyframeSequence") == 0 ||
                   strcmp(class_name, "javax/microedition/m3g/AnimationController") == 0 ||
                   strcmp(class_name, "javax/microedition/m3g/AnimationTrack") == 0 ||
                   strcmp(class_name, "javax/microedition/m3g/Fog") == 0 ||
                   strcmp(class_name, "javax/microedition/m3g/CompositingMode") == 0 ||
                   strcmp(class_name, "javax/microedition/m3g/PolygonMode") == 0) {
            super_name = "javax/microedition/m3g/Object3D";
        }
        /* Other M3G classes extend Object */
    }
    
    /* FIX-19v: generic exception/error fallback for unknown classes.
     * Without this, classes like java/lang/StringIndexOutOfBoundsException
     * (not in the explicit table) got super=java/lang/Object, so
     * object_instance_of(sioobe, Exception) failed and exception handlers
     * like 'catch (Exception e)' NEVER matched - GlomoUtil.JAD's internal
     * safe-wrapper catch was skipped and SU-30's startApp died. */
    else if (strstr(class_name, "Exception") == class_name + strlen(class_name) - strlen("Exception")) {
        super_name = "java/lang/Exception";
    }
    else if (strstr(class_name, "Error") == class_name + strlen(class_name) - strlen("Error")) {
        super_name = "java/lang/Error";
    }
    else if (strstr(class_name, "Throwable") == class_name + strlen(class_name) - strlen("Throwable")) {
        super_name = "java/lang/Throwable";
    }
    
    /* Create stub class with proper super class */
    JavaClass* stub = create_stub_class(jvm, class_name, super_name, ACC_PUBLIC);
    if (!stub) return NULL;
    
    /* Ensure class_loader has capacity */
    if (jvm->class_loader.count >= jvm->class_loader.capacity) {
        size_t new_cap = jvm->class_loader.capacity ? jvm->class_loader.capacity * 2 : 32;
        jvm->class_loader.classes = (JavaClass**)realloc(jvm->class_loader.classes,
                               new_cap * sizeof(JavaClass*));
        if (!jvm->class_loader.classes) {
            free(stub);
            return NULL;
        }
        jvm->class_loader.capacity = new_cap;
    }
    
    jvm->class_loader.classes[jvm->class_loader.count++] = stub;
    
    /* Resolve super_class reference */
    if (stub->super_class == NULL && stub->super_class_name != NULL) {
        /* Try to find super class in loaded classes */
        for (size_t j = 0; j < jvm->class_loader.count; j++) {
            if (jvm->class_loader.classes[j]->class_name &&
                strcmp(jvm->class_loader.classes[j]->class_name, stub->super_class_name) == 0) {
                stub->super_class = jvm->class_loader.classes[j];
                break;
            }
        }
        /* If not found, create it recursively */
        if (stub->super_class == NULL && strcmp(stub->super_class_name, "java/lang/Object") != 0) {
            stub->super_class = get_or_create_stub_class(jvm, stub->super_class_name);
        }
    }
    
    /* Add interfaces for specific classes */
    stub_add_known_interfaces(stub);

    /* Initialize header for Class object support */
    JavaClass* class_class = NULL;
    for (size_t i = 0; i < jvm->class_loader.count; i++) {
        if (jvm->class_loader.classes[i]->class_name &&
            strcmp(jvm->class_loader.classes[i]->class_name, "java/lang/Class") == 0) {
            class_class = jvm->class_loader.classes[i];
            break;
        }
    }
    if (class_class) {
        stub->header.clazz = class_class;
        stub->header.hashcode = (jint)(uintptr_t)stub ^ 0x5A5A5A5A;
    }

    if (jvm->config.verbose_class) {
        CLASS_DEBUG(" Auto-created stub class: %s (super: %s)", class_name, super_name);
    }

    return stub;
}