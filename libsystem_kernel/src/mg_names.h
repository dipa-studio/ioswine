/*
 * mg_names.h: the ONE wire-string table for the MobileGestalt service.
 *
 * Same pattern as fakeboard_names.h: every service name, question
 * string, obfuscated cache key, reply key and cache path component
 * the mg service uses lives here and nowhere else, so a wrong string
 * can be corrected after the first device trace without touching any
 * logic. No wire-string literal appears in mg.c.
 *
 * Every obfuscated key below was verified against the formula
 * (base64(md5("MGCopyAnswer" + question)) minus trailing "=="):
 * 49 of 49 known question/key pairs reproduce exactly.
 * Two keys were computed with the same formula: MetalCapability
 * and BluetoothAddress. The one key the formula does NOT reproduce
 * is the marketing-name anomaly Z/dqyWS6OZTRy10UcmUAhw, which is
 * copied verbatim (its plaintext question is unknown).
 *
 * [src] tags point at the DeepWiki MobileGestalt plist page.
 * [guess] marks values no public source pins for iPhone17,3.
 *
 * No em dashes are used in this file.
 */
#pragma once

/* Mach service name. */
#define MG_SERVICE_NAME "com.apple.mobilegestalt.xpc"

/* Cache file location. The group
 * id is the directory name under Shared/SystemGroup on a real
 * device; container_group() maps it. */
#define MG_CACHE_GROUP_ID "systemgroup.com.apple.mobilegestaltcache"
#define MG_CACHE_REL_PATH "Library/Caches/com.apple.MobileGestalt.plist"

/* Verbatim-trace prefix for bsxpc_log_dict. */
#define MG_TRACE_PREFIX "[mg]"

/* Top-level cache keys. */
#define MG_KEY_CACHE_DATA "CacheData"
#define MG_KEY_CACHE_EXTRA "CacheExtra"
#define MG_KEY_CACHE_UUID "CacheUUID"
#define MG_KEY_CACHE_VERSION "CacheVersion"

/* CacheVersion must equal the running build or the cache is treated
 * as stale. */
#define MG_CACHE_VERSION "23G90"

/* CacheUUID: the fixed value the bypass-device library reuses across
 * all its files [src: DeepWiki MobileGestalt plist page]. No public
 * source says the UUID is validated against hardware [guess]. */
#define MG_CACHE_UUID "CEEA7316-5800-454A-AA6A-834513AAC59B"

/* Request and reply keys. The plain-dict request shape
 * {"question": "<question>"} is the only shape buildable without a
 * device trace and is probably wrong for the real NSXPC helper
 * [guess]. The reply key "answer" is likewise [guess]. The "error"
 * key follows the svchost handler interface (svchost.h). */
#define MG_REQ_KEY_QUESTION "question"
#define MG_REPLY_KEY_ANSWER "answer"
#define MG_REPLY_KEY_ERROR "error"

/* ------------------------------------------------------------------ */
/* Launch-time questions: question string, obfuscated CacheExtra key, */
/* and string value where the answer is a string. Values are for      */
/* iPhone17,3 / iOS 26.6.2 build 23G90, with main-screen answers      */
/* matching the BKS fake (1080x2400 @ 3.0), not the iPhone panel.     */
/* ------------------------------------------------------------------ */

#define MG_Q_DEVICE_CLASS "DeviceClass"
#define MG_K_DEVICE_CLASS "+3Uf0Pm5F8Xy7Onyvko0vA"
#define MG_V_DEVICE_CLASS "iPhone"

#define MG_Q_DEVICE_CLASS_NUMBER "DeviceClassNumber"
#define MG_K_DEVICE_CLASS_NUMBER "mtrAoWJ3gsq+I90ZnQ0vQw"

#define MG_Q_DEVICE_NAME "DeviceName"
#define MG_K_DEVICE_NAME "rkqlwPcRHwixY4gapPjanw"
#define MG_V_DEVICE_NAME "iPhone"

#define MG_Q_GS_DEVICE_NAME "GSDeviceName"
#define MG_K_GS_DEVICE_NAME "9s45ldrCC1WF+7b6C4H2BA"
#define MG_V_GS_DEVICE_NAME "iPhone"

#define MG_Q_PRODUCT_TYPE "ProductType"
#define MG_K_PRODUCT_TYPE "h9jDsbgj7xIVeIQ8S3/X3Q"
#define MG_V_PRODUCT_TYPE "iPhone17,3"

#define MG_Q_PRODUCT_VERSION "ProductVersion"
#define MG_K_PRODUCT_VERSION "qNNddlUK+B/YlooNoymwgA"
#define MG_V_PRODUCT_VERSION "26.6.2"

#define MG_Q_BUILD_VERSION "BuildVersion"
#define MG_K_BUILD_VERSION "mZfUC7qo4pURNhyMHZ62RQ"
#define MG_V_BUILD_VERSION "23G90"

#define MG_Q_PRODUCT_NAME "ProductName"
#define MG_K_PRODUCT_NAME "ivIu8YTDnBSrYv/SN4G8Ag"
#define MG_V_PRODUCT_NAME "iPhone OS"

#define MG_Q_HW_MODEL_STR "HWModelStr"
#define MG_K_HW_MODEL_STR "/YYygAofPDbhrwToVsXdeA"
#define MG_V_HW_MODEL_STR "D47AP"

#define MG_Q_IS_SIMULATOR "IsSimulator"
#define MG_K_IS_SIMULATOR "ulMliLomP737aAOJ/w/evA"

#define MG_Q_INTERNAL_BUILD "InternalBuild"
#define MG_K_INTERNAL_BUILD "LBJfwOEzExRxzlAnSuI7eg"

/* Same question as InternalBuild; key computed with the formula. */
#define MG_Q_APPLE_INTERNAL_INSTALL "AppleInternalInstallCapability"
#define MG_K_APPLE_INTERNAL_INSTALL "9ZQAcDmoSgmTrmVL06N7hA"

#define MG_Q_CPU_ARCHITECTURE "CPUArchitecture"
#define MG_K_CPU_ARCHITECTURE "k7QIBwZJJOVw+Sej/8h8VA"
#define MG_V_CPU_ARCHITECTURE "arm64e"

#define MG_Q_SUPPORTED_DEVICE_FAMILIES "SupportedDeviceFamilies"
#define MG_K_SUPPORTED_DEVICE_FAMILIES "9MZ5AdH43csAUajl/dU+IQ"

#define MG_Q_REGION_CODE "RegionCode"
#define MG_K_REGION_CODE "h63QSdBCiT/z0WU6rdQv6Q"
#define MG_V_REGION_CODE "LL"

#define MG_Q_REGION_INFO "RegionInfo"
#define MG_K_REGION_INFO "zHeENZu+wbg7PUprwNwBWg"
#define MG_V_REGION_INFO "LL/A"

#define MG_Q_REGULATORY_MODEL_NUMBER "RegulatoryModelNumber"
#define MG_K_REGULATORY_MODEL_NUMBER "97JDvERpVwO+GHtthIh7hA"
#define MG_V_REGULATORY_MODEL_NUMBER "A3287"

#define MG_Q_MODEL_NUMBER "ModelNumber"
#define MG_K_MODEL_NUMBER "D0cJ8r7U5zve6uA6QbOiLA"
/* Sales model code; MYEC3 is the order-number prefix for the global
 * iPhone 16, the exact MG value is [guess]. */
#define MG_V_MODEL_NUMBER "MYEC3"

/* Anomaly key: holds the marketing name in real caches, but the
 * formula applied to MarketingNameString or MarketingName does not
 * reproduce it. Copied verbatim; value is the iPhone 16 name. */
#define MG_Q_MARKETING_NAME "MarketingNameString"
#define MG_K_MARKETING_NAME "Z/dqyWS6OZTRy10UcmUAhw"
#define MG_V_MARKETING_NAME "iPhone 16"

#define MG_Q_DEVICE_COLOR_MAP_POLICY "DeviceColorMapPolicy"
#define MG_K_DEVICE_COLOR_MAP_POLICY "87sSAh2rboMI2TDvFBimkg"

#define MG_Q_HAS_BASEBAND "HasBaseband"
#define MG_K_HAS_BASEBAND "AJFQheZDyUbvI6RmBMT9Cg"

#define MG_Q_CONTAINS_CELLULAR_RADIO "contains-cellular-radio"
#define MG_K_CONTAINS_CELLULAR_RADIO "yRZv0s7Dpj8ZBk0S+0+nMA"

#define MG_Q_CELLULAR_TELEPHONY "CellularTelephonyCapability"
#define MG_K_CELLULAR_TELEPHONY "ebyBs0j3KAquBsgcfrNZIg"

#define MG_Q_IPAD "ipad"
#define MG_K_IPAD "uKc7FPnEO++lVhHWHFlGbQ"

#define MG_Q_ARM64 "arm64"
#define MG_K_ARM64 "kKgJsWN/rBUAkimOtm/wbA"

#define MG_Q_DEVICE_SUPPORTS_64BIT "DeviceSupports64Bit"
#define MG_K_DEVICE_SUPPORTS_64BIT "dvaktw5A/h0zlsWpMg/COQ"

#define MG_Q_DEVICE_SUPPORTS_ARKIT "DeviceSupportsARKit"
#define MG_K_DEVICE_SUPPORTS_ARKIT "iAu0GZogf4TG69GSO5rHcg"

/* Key computed with the section 2.3 formula; the doc left it
 * uncomputed. iPhone 16 supports ARKit and Metal by hardware. */
#define MG_Q_METAL_CAPABILITY "MetalCapability"
#define MG_K_METAL_CAPABILITY "/8aKho3RQEvq1PxPnN2ziQ"

#define MG_Q_HAS_EXTENDED_COLOR_DISPLAY "HasExtendedColorDisplay"
#define MG_K_HAS_EXTENDED_COLOR_DISPLAY "Aixt/MEN2O2B7f+8m4TxUA"

#define MG_Q_DEVICE_CORNER_RADIUS "DeviceCornerRadius"
#define MG_K_DEVICE_CORNER_RADIUS "y5dppxx/LzxoNuW+iIKR3g"

#define MG_Q_SUPPORTS_FORCE_TOUCH "SupportsForceTouch"
#define MG_K_SUPPORTS_FORCE_TOUCH "eQd5mlz0BN0amTp/2ccMoA"

#define MG_Q_MAIN_DISPLAY_ROTATION "MainDisplayRotation"
#define MG_K_MAIN_DISPLAY_ROTATION "IIYjwu1lbAyGz5t0c4ECoA"

#define MG_Q_HAS_SPRINGBOARD "HasSpringBoard"
#define MG_K_HAS_SPRINGBOARD "OBqqs000I0SR+EbJ7VO8UQ"

#define MG_Q_COMPATIBLE_APP_VARIANTS "CompatibleAppVariants"
#define MG_K_COMPATIBLE_APP_VARIANTS "VuGdqp8UBpi9vPWHlPluVQ"
/* [guess] previous-gen same-tier model is iPhone 15 (iPhone16,1). */
#define MG_V_COMPATIBLE_APP_VARIANT_0 "iPhone17,3"
#define MG_V_COMPATIBLE_APP_VARIANT_1 "iPhone16,1"

/* Target device color is unknown; any well-formed hex string is
 * accepted by callers [guess]. Values below are the iPad Air 2
 * sample values from Levin, used as placeholders. */
#define MG_Q_DEVICE_ENCLOSURE_COLOR "DeviceEnclosureColor"
#define MG_K_DEVICE_ENCLOSURE_COLOR "JhEU414EIaDvAz8ki5DSqw"
#define MG_V_DEVICE_ENCLOSURE_COLOR "#e1ccb5"

#define MG_Q_DEVICE_COLOR "DeviceColor"
#define MG_K_DEVICE_COLOR "mumHZHMLEfAuTkkd28fHlQ"
#define MG_V_DEVICE_COLOR "#e1e4e3"

/* Main-screen answers match the BKS fake (1080x2400 @ 3.0), not the
 * iPhone panel (1179x2556 @ 3.0). Pitch 391 is the Xiaomi 12 Pro
 * panel density [guess]; class 8 is
 * the most recent iPhone data point (iPhone 6) [guess]. */
#define MG_Q_MAIN_SCREEN_WIDTH "main-screen-width"
#define MG_K_MAIN_SCREEN_WIDTH "g7YQ1Djxh4YiKlEeaoGhzg"

#define MG_Q_MAIN_SCREEN_HEIGHT "main-screen-height"
#define MG_K_MAIN_SCREEN_HEIGHT "OjzOua0LkOegX7pQdgMksw"

#define MG_Q_MAIN_SCREEN_SCALE "main-screen-scale"
#define MG_K_MAIN_SCREEN_SCALE "SNfDJgQFV2Xj7+WnozcJPw"

#define MG_Q_MAIN_SCREEN_PITCH "main-screen-pitch"
#define MG_K_MAIN_SCREEN_PITCH "0l4wqBtWEAK1tOkeBHkU6Q"

#define MG_Q_MAIN_SCREEN_CLASS "main-screen-class"
#define MG_K_MAIN_SCREEN_CLASS "fdh+s6j3VijuyrK7xLjd7g"

#define MG_Q_MAIN_SCREEN_STATIC_INFO "MainScreenStaticInfo"
#define MG_K_MAIN_SCREEN_STATIC_INFO "oBbtJ8x+s1q0OkaiocPuog"

#define MG_Q_MAIN_SCREEN_ORIENTATION "MainScreenOrientation"
#define MG_K_MAIN_SCREEN_ORIENTATION "1h6a0FG2nwMsUDVnYfaxxA"

#define MG_Q_MAIN_SCREEN_CANVAS_SIZES "MainScreenCanvasSizes"
#define MG_K_MAIN_SCREEN_CANVAS_SIZES "ybGkijAwLTwevankfVzsDQ"

/* Artwork sub-dict key. */
#define MG_K_ARTWORK_TRAITS "oPeik/9e8lQWMszEjbPzng"
#define MG_Q_ARTWORK_TRAITS "ArtworkTraits"

/* ------------------------------------------------------------------ */
/* Cache-only duplicate keys: Apple stores the same                  */
/* value under several obfuscated keys. These keys have no known      */
/* plaintext question; they are filled in the cache file only.        */
/* [src: DeepWiki MobileGestalt plist page].                           */
/* ------------------------------------------------------------------ */

/* ProductType duplicates (8 beyond the canonical key). */
#define MG_K_DUP_PRODUCT_TYPE_0 "xNN67KktpWp7syTT3S1BFA"
#define MG_K_DUP_PRODUCT_TYPE_1 "+1TeoctsaQC55zwHZ6MESg"
#define MG_K_DUP_PRODUCT_TYPE_2 "myx96YOqBSDzLwljSYWBiQ"
#define MG_K_DUP_PRODUCT_TYPE_3 "0+nc/Udy4WNG8S+Q7a/s1A"
#define MG_K_DUP_PRODUCT_TYPE_4 "GEsznZwAYGOa1a67QU1Uew"
#define MG_K_DUP_PRODUCT_TYPE_5 "GqAdWRLnC7oYQrNYF48VYA"
#define MG_K_DUP_PRODUCT_TYPE_6 "MKE8hwsOxxRCtwBk2aDBZA"
#define MG_K_DUP_PRODUCT_TYPE_7 "G91h5IuJvXISeyngNFqEpg"

/* DeviceClass duplicates (2 beyond DeviceClass/DeviceName/
 * GSDeviceName). */
#define MG_K_DUP_DEVICE_CLASS_0 "vme9Buk6XiWFCXoHApxNFA"
#define MG_K_DUP_DEVICE_CLASS_1 "JUWcn+5Ss0nvr5w/jk4WEg"

/* HWModelStr duplicates (8 beyond the canonical key). */
#define MG_K_DUP_HW_MODEL_STR_0 "oQNDePXjSD1z7W0ddqt9tg"
#define MG_K_DUP_HW_MODEL_STR_1 "dW5fpt/6HhaTbnK/UqL6cA"
#define MG_K_DUP_HW_MODEL_STR_2 "GGIIDN/ANr8X2WrgS6nBYQ"
#define MG_K_DUP_HW_MODEL_STR_3 "ZGraRMW0TsxCvONeeJ5C2w"
#define MG_K_DUP_HW_MODEL_STR_4 "uCIk6n9Am5fsV2cTjhqFQw"
#define MG_K_DUP_HW_MODEL_STR_5 "b4e7mEbjqfewD6oXmo9U5g"
#define MG_K_DUP_HW_MODEL_STR_6 "oYicEKzVTz4/CxxE05pEgQ"
#define MG_K_DUP_HW_MODEL_STR_7 "yAfB6E2v0++rHtdW7SDg8w"

/* Marketing-name duplicate (1 beyond the anomaly key). */
#define MG_K_DUP_MARKETING_NAME_0 "bbtR9jQx50Fv5Af/affNtA"

/* Opaque binary field holding an all-zero 52-byte buffer in real
 * caches; its question is unknown [src: DeepWiki]. */
#define MG_K_ZERO_52 "91LyMcx4z1w3SGVeqteMnA"

/* GPU buffer-compression sub-dict key. */
#define MG_K_GPU_COMPRESSION "AoKnINTLPoKML3ctoP0AZg"

/* ------------------------------------------------------------------ */
/* Artwork sub-dict keys (human-readable, inside CacheExtra under    */
/* MG_K_ARTWORK_TRAITS).                                              */
/* ------------------------------------------------------------------ */

#define MG_ART_DEVICE_IDIOM "ArtworkDeviceIdiom"
#define MG_V_ART_DEVICE_IDIOM "phone"
#define MG_ART_DEVICE_SUBTYPE "ArtworkDeviceSubType"
/* [guess] 2778 is the iPhone13,4 value; iPhone17,3 is unpublished. */
#define MG_V_ART_DEVICE_SUBTYPE 2778
#define MG_ART_DEVICE_SCALE_FACTOR "ArtworkDeviceScaleFactor"
#define MG_V_ART_DEVICE_SCALE_FACTOR 3
#define MG_ART_DISPLAY_GAMUT "ArtworkDisplayGamut"
#define MG_V_ART_DISPLAY_GAMUT "P3"
#define MG_ART_DYNAMIC_DISPLAY_MODE "ArtworkDynamicDisplayMode"
#define MG_V_ART_DYNAMIC_DISPLAY_MODE "0"
#define MG_ART_PRODUCT_DESCRIPTION "ArtworkDeviceProductDescription"
#define MG_V_ART_PRODUCT_DESCRIPTION "iPhone 16"
#define MG_ART_GRAPHICS_FEATURE_SET_CLASS "GraphicsFeatureSetClass"
/* [guess] iPhone13,4 is APPLE7; the A18 class is unpublished. */
#define MG_V_ART_GRAPHICS_FEATURE_SET_CLASS "APPLE8"
#define MG_ART_GRAPHICS_FEATURE_SET_FALLBACKS "GraphicsFeatureSetFallbacks"
/* [guess] follows the APPLE7 pattern. */
#define MG_V_ART_GRAPHICS_FEATURE_SET_FALLBACKS \
    "APPLE7:APPLE6:APPLE5:APPLE4:APPLE3:APPLE2:APPLE1:GLES2,0"
#define MG_ART_COMPATIBLE_DEVICE_FALLBACK "CompatibleDeviceFallback"
/* [guess] previous-gen same-tier model. */
#define MG_V_ART_COMPATIBLE_DEVICE_FALLBACK "iPhone16,1"

/* ------------------------------------------------------------------ */
/* GPU buffer-compression sub-dict keys (human-readable, inside       */
/* CacheExtra under MG_K_GPU_COMPRESSION). Values are the iPhone13,4  */
/* values [src: DeepWiki]; the A18 values are [guess].                */
/* ------------------------------------------------------------------ */

#define MG_GPU_BUFFER_COMPRESSION "buffer-compression"
#define MG_GPU_MEDIA_COMPRESSION "media-compression"
#define MG_GPU_UNIVERSAL_BUFFER_COMPRESSION "universal-buffer-compression"
#define MG_GPU_UNIVERSAL_BUFFER_COMPRESSION_ALIGNMENT_2048 \
    "universal-buffer-compression-requires-plane-alignment-2048"
#define MG_GPU_UNIVERSAL_LOSSY_BUFFER_COMPRESSION \
    "universal-lossy-buffer-compression"
#define MG_GPU_UNIVERSAL_MULTISLICE_BUFFER_COMPRESSION \
    "universal-multislice-buffer-compression"
#define MG_GPU_UNIVERSAL_PARGB10101010_BUFFER_COMPRESSION \
    "universal-pARGB10101010-buffer-compression"

/* ------------------------------------------------------------------ */
/* Protected questions: denied, never fabricated. The                 */
/* BluetoothAddress, UniqueChipID and MobileEquipmentIdentifier keys  */
/* were computed with the formula.                                    */
/* ------------------------------------------------------------------ */

#define MG_Q_UNIQUE_DEVICE_ID "UniqueDeviceID"
#define MG_K_UNIQUE_DEVICE_ID "re6Zb+zwFKJNlkQTUeT+/w"
#define MG_Q_UNIQUE_DEVICE_ID_DATA "UniqueDeviceIDData"
#define MG_K_UNIQUE_DEVICE_ID_DATA "nFRqKto/RuQAV1P+0/qkBA"
#define MG_Q_SERIAL_NUMBER "SerialNumber"
#define MG_K_SERIAL_NUMBER "VasUgeSzVyHdB27g2XpN0g"
#define MG_Q_MLB_SERIAL_NUMBER "MLBSerialNumber"
#define MG_K_MLB_SERIAL_NUMBER "Q1Ty5w8gxMWHx3p4lQ1fhA"
#define MG_Q_BLUETOOTH_ADDRESS "BluetoothAddress"
#define MG_K_BLUETOOTH_ADDRESS "k5lVWbXuiZHLA17KGiVUAA"
#define MG_Q_INVERSE_DEVICE_ID "InverseDeviceID"
#define MG_K_INVERSE_DEVICE_ID "frZQaeyWLUvLjeuEK43hmg"
#define MG_Q_UNIQUE_CHIP_ID "UniqueChipID"
#define MG_K_UNIQUE_CHIP_ID "TF31PAB6aO8KAbPyNKSxKA"
#define MG_Q_MOBILE_EQUIPMENT_ID "MobileEquipmentIdentifier"
#define MG_K_MOBILE_EQUIPMENT_ID "xOEH0P1H/1jmYe2t54+5cQ"

/* ------------------------------------------------------------------ */
/* Dynamic questions: no static answer; the helper returns NULL.     */
/* Keys computed with the formula; the cache file must not contain   */
/* them.                                                              */
/* ------------------------------------------------------------------ */

#define MG_Q_BATTERY_CURRENT_CAPACITY "BatteryCurrentCapacity"
#define MG_K_BATTERY_CURRENT_CAPACITY "f2DlVMUVcV+MeWs/g2ku+g"
#define MG_Q_BATTERY_IS_CHARGING "BatteryIsCharging"
#define MG_K_BATTERY_IS_CHARGING "sLe8lqXRlqZM74MNUoVcyQ"
#define MG_Q_AIRPLANE_MODE "AirplaneMode"
#define MG_K_AIRPLANE_MODE "l/n0Z7pMB8k0GyTKz3v3Tw"
#define MG_Q_SIM_STATUS "SIMStatus"
#define MG_K_SIM_STATUS "yUCaqT4KOwJpYEb+XDPq7g"
#define MG_Q_ACTIVE_WIRELESS_TECHNOLOGY "ActiveWirelessTechnology"
#define MG_K_ACTIVE_WIRELESS_TECHNOLOGY "Eavu9TP+iZ0Lzlk7FU6w6Q"
