/*
 * fakeboard_names.h: the ONE wire-string table for the fake FrontBoard.
 *
 * Every service name, selector, message name, envelope key, settings
 * key, identity string and display value the fake uses lives here and
 * nowhere else. fakeboard.c and fakerb.c must use only this header for
 * wire strings, so a wrong string can be corrected after verification
 * without touching any logic. No
 * logic lives in this file, only #defines.
 *
 * Tag on each entry: [src: ...] means the string (or the value) was
 * read from that public source or from static disassembly of the
 * iOS 26.6.2 (23G90) cache.
 * [guess] means inference, not verified; the string is our best
 * prediction of the wire form and must be checked against device traces.
 * Where a name is a property name from a header but
 * its use as a wire key is unproven, both are noted.
 *
 * No em dashes are used in this file.
 */
#pragma once

/* ------------------------------------------------------------------ */
/* Mach service names.                                                 */
/* ------------------------------------------------------------------ */

/* String dump of the 26.6.2 cache; FBSWorkspaceServiceSpecification
 * identifier. The scene-creation endpoint. */
#define FB_SVC_WORKSPACE_SERVICE "com.apple.frontboard.workspace-service"
/* FBSWorkspace -_initWithCoupler:options:. May be the same endpoint
 * under the coupler name, or a second connection [guess]. */
#define FB_SVC_WORKSPACE_COUPLER "com.apple.frontboardservices.workspace"
/* String dump; also in the SpringBoard and UIKitSystem launchd plists
 * [src: https://adamdemasi.com/2018/06/07/iosmac-research]. The
 * workspace identifier seen in scene identities. */
#define FB_SVC_SYSTEMAPP_SERVICES "com.apple.frontboard.systemappservices"
/* FBSSystemServiceSpecification identifier. Exact messages unknown [guess]. */
#define FB_SVC_SYSTEM_SERVICE "com.apple.frontboard.system-service"

/* Number of services fakeboard_init registers, in the order above. */
#define FB_NUM_SERVICES 4

/* ------------------------------------------------------------------ */
/* Selectors and message names.                                        */
/* ------------------------------------------------------------------ */

/* Scene-create callout selector, client side (FBSWorkspaceScenesClient).
 * Verified against iOS 26.6.2 (23G90) dsc:
 * -[FBSWorkspaceScenesClient createSceneWithIdentity:parameters:transitionContext:completion:]
 * [src: 0x1a475d6f0] in FBSWorkspaceServiceServerInterface [src: 0x1eca1c9c8]. */
#define FB_MSG_CREATE_SCENE_NEW "createWithSceneID:groupID:parameters:transitionContext:completion:"
#define FB_MSG_CREATE_SCENE_OLD "createSceneWithIdentity:parameters:transitionContext:completion:"
#define FB_MSG_CREATE_SCENE FB_MSG_CREATE_SCENE_OLD
/* Verified workspace service handshake & update selectors [src: 0x1eca1ca88, 0x1eca1c9c8]. */
#define FB_MSG_HANDSHAKE_WITH_REMNANTS "handshakeWithRemnants:"
#define FB_MSG_UPDATE_SETTINGS_DIFF "sceneID:updateWithSettingsDiff:transitionContext:completion:"
#define FB_CTX_KEY_MESSAGE "message"
#define FB_CTX_KEY_SERVICE "s"

/* App-side scene-create request. The ObjC selector is
 * -requestSceneCreationWithIdentifier:initialClientSettings:completion:
 * [src: https://developer.limneos.net/?ios=18.1&framework=FrontBoardServices.framework&header=FBSWorkspace.h].
 * The short wire message name is [guess]. */
#define FB_MSG_REQUEST_SCENE_CREATION "requestSceneCreation"
/* Server-side service object methods. The ObjC selectors are
 * -createScene:completion: and -requestSceneWithOptions:completion:
 * [src: https://developer.limneos.net/?ios=18.1&framework=FrontBoardServices.framework&header=FBSWorkspaceService.h].
 * The short wire names are [guess]. */
#define FB_MSG_CREATE_SCENE_REQUEST "createScene"
#define FB_MSG_REQUEST_SCENE "requestScene"
/* Settings push and lifecycle callouts. The client-side methods are
 * -updateSettings:, -updateSettingsWithTransitionContext:,
 * -activateWithCompletion: and -invalidate
 * [src: https://developer.limneos.net/?ios=18.1&framework=FrontBoardServices.framework&header=FBSScene.h].
 * The wire message names are [guess]. */
#define FB_MSG_UPDATE_SETTINGS "updateSettings"
#define FB_MSG_ACTIVATE "activate"
#define FB_MSG_INVALIDATE "invalidate"
#define FB_MSG_DEACTIVATE "deactivate"
/* Client acknowledgement that it created the scene. Wire name [guess]. */
#define FB_MSG_DID_CREATE_SCENE "didCreateScene"
/* FBSSystemService methods
 * [src: https://developer.limneos.net/?ios=18.1&framework=FrontBoardServices.framework&header=FBSSystemService.h].
 * Wire message names [guess]. */
#define FB_MSG_OPEN_APPLICATION "openApplication"
#define FB_MSG_PID_FOR_APPLICATION "pidForApplication"
#define FB_MSG_PROCESS_HANDLE_FOR_APPLICATION "processHandleForApplication"
#define FB_MSG_TERMINATE_APPLICATION "terminateApplication"

/* BSAction kinds for lifecycle actions. The BSAction properties are
 * info, handler, kind, listener, responder, target, timeout
 * [src: https://developer.limneos.net/?ios=18.1&framework=BaseBoard.framework&header=BSAction.h].
 * The kind strings are [guess]. */
#define FB_ACTION_ACTIVATE "activate"
#define FB_ACTION_DEACTIVATE "deactivate"
#define FB_ACTION_DESTROY "destroy"

/* ------------------------------------------------------------------ */
/* BSXPC envelope keys. Measured on device 2026-11-12.                */
/* ------------------------------------------------------------------ */

#define FB_BSXPC_KEY_BSXPC "bsxpc"
#define FB_BSXPC_KEY_CONTEXT "bsxpc_context"
#define FB_BSXPC_KEY_CID "bsxpc_CID"
#define FB_BSXPC_KEY_CIDR "bsxpc_CIDr"
#define FB_BSXPC_KEY_SEL "bsxpc_SEL"

/* Connection handshake kind and the initialize selector, both from
 * the device trace (2026-11-12). */
#define FB_MSG_CONNECT "connect"
#define FB_MSG_INITIALIZE_CLIENT "initializeClientWithParameters:completion:"

/* Full scene-create request selector, the ObjC selector
 * -requestSceneCreationWithIdentifier:initialClientSettings:completion:
 * [src: https://developer.limneos.net/?ios=18.1&framework=FrontBoardServices.framework&header=FBSWorkspace.h].
 * The client requests
 * a scene with this selector, so the fake matches on it as well as
 * on the short [guess] names below. */
#define FB_MSG_REQUEST_SCENE_CREATION_FULL "requestSceneCreationWithIdentifier:initialClientSettings:completion:"

/* BSAction field keys. Property names from BSAction.h (see above);
 * their use as wire keys is [guess]. */
#define FB_BSXPC_KEY_ACTION_KIND "kind"
#define FB_BSXPC_KEY_ACTION_INFO "info"
#define FB_BSXPC_KEY_ACTION_TARGET "target"
#define FB_BSXPC_KEY_ACTION_TIMEOUT "timeout"
#define FB_BSXPC_KEY_ACTION_RESPONSE "response"

/* Process-handle field keys. Property names from RBSProcessHandle.h
 * [src: https://developer.limneos.net/?ios=18.1&framework=RunningBoardServices.framework&header=RBSProcessHandle.h];
 * their use as wire keys is [guess]. */
#define FB_BSXPC_KEY_HANDLE_PID "pid"
#define FB_BSXPC_KEY_HANDLE_BUNDLE_ID "bundleID"
#define FB_BSXPC_KEY_HANDLE_AUDIT_TOKEN "auditToken"
#define FB_BSXPC_KEY_HANDLE_IS_RUNNING "isRunning"
#define FB_BSXPC_KEY_HANDLE_IS_FOREGROUND "isForeground"
#define FB_BSXPC_KEY_HANDLE_IS_VALID "isValid"
#define FB_BSXPC_KEY_HANDLE_IS_APPLICATION "isApplication"

/* ------------------------------------------------------------------ */
/* Callout argument keys. All [guess]. */
/* ------------------------------------------------------------------ */

#define FB_ENV_IDENTITY "identity"
#define FB_ENV_PARAMETERS "parameters"
#define FB_ENV_SETTINGS "settings"
#define FB_ENV_CLIENT_SETTINGS "clientSettings"
#define FB_ENV_TRANSITION_CONTEXT "transitionContext"
#define FB_ENV_SPECIFICATION "specification"
#define FB_ENV_SCENE_IDENTIFIER "sceneIdentifier"
#define FB_ENV_BUNDLE_ID "bundleID"
#define FB_ENV_PID "pid"
#define FB_ENV_PROCESS_HANDLE "processHandle"
/* Reply / error key. The "error" key is measured, not guessed. */
#define FB_ENV_ERROR "error"

/* FBSSceneIdentity fields. Property names
 * [src: https://developer.limneos.net/?ios=18.1&framework=FrontBoardServices.framework&header=FBSSceneIdentity.h];
 * their use as wire keys is [guess]. */
#define FB_IDENTITY_IDENTIFIER "identifier"
#define FB_IDENTITY_WORKSPACE_IDENTIFIER "workspaceIdentifier"
#define FB_IDENTITY_UNIQUE_IDENTIFIER "uniqueIdentifier"
#define FB_IDENTITY_INTERNAL_IDENTIFIER "internalIdentifier"

/* ------------------------------------------------------------------ */
/* Scene identity and specification.                                   */
/* ------------------------------------------------------------------ */

/* Scene identifier string "sceneID:<bundleID>-default"
 * [src: https://github.com/khanhduytran0/FrontBoardAppLauncher].
 * The workspace identifier in the combined identity form
 * "com.apple.frontboard.systemappservices::<bundleID>-default"
 * [src: https://pastebin.com/T8ZR8WwB]. */
#define FB_SCENE_ID_PREFIX "sceneID:"
#define FB_SCENE_ID_SUFFIX "-default"
/* Scene specification
 * [src: https://github.com/khanhduytran0/FrontBoardAppLauncher]. */
#define FB_SPEC_UIAPPLICATION_SCENE "UIApplicationSceneSpecification"

/* ------------------------------------------------------------------ */
/* FBSSceneSettings keys. Property names from FBSSceneSettings.h      */
/* [src: https://developer.limneos.net/?ios=18.1&framework=FrontBoardServices.framework&header=FBSSceneSettings.h]; */
/* their use as wire keys is [guess] (frontboard-scene.md section 4:  */
/* the BSXPC coder's on-the-wire key strings are not in any header).  */
/* ------------------------------------------------------------------ */

#define FB_SET_DISPLAY_CONFIGURATION "displayConfiguration"
#define FB_SET_FRAME "frame"
#define FB_SET_INTERFACE_ORIENTATION "interfaceOrientation"
#define FB_SET_DEVICE_ORIENTATION "deviceOrientation"
#define FB_SET_FOREGROUND "foreground"
#define FB_SET_BACKGROUNDED "backgrounded"
#define FB_SET_LEVEL "level"
#define FB_SET_PERSISTENCE_IDENTIFIER "persistenceIdentifier"
#define FB_SET_PERIPHERY_INSETS "peripheryInsets"
#define FB_SET_SAFE_AREA_INSETS_PORTRAIT "safeAreaInsetsPortrait"
#define FB_SET_STATUS_BAR_DISABLED "statusBarDisabled"
#define FB_SET_CAN_SHOW_ALERTS "canShowAlerts"
#define FB_SET_CORNER_RADIUS_CONFIGURATION "cornerRadiusConfiguration"
#define FB_SET_CORNER_RADIUS "radius"

/* FBSSceneClientSettings properties
 * [src: https://developer.limneos.net/?ios=18.1&framework=FrontBoardServices.framework&header=FBSSceneClientSettings.h];
 * wire keys [guess]. */
#define FB_SET_PREFERRED_INTERFACE_ORIENTATION "preferredInterfaceOrientation"
#define FB_SET_LAYERS "layers"
#define FB_SET_PREFERRED_SCENE_HOST_IDENTITY "preferredSceneHostIdentity"
#define FB_SET_PREFERRED_SCENE_HOST_IDENTIFIER "preferredSceneHostIdentifier"
#define FB_SET_STATUS_BAR_STYLE "statusBarStyle"

/* FBSDisplayConfiguration properties
 * [src: https://developer.limneos.net/?ios=18.1&framework=FrontBoardServices.framework&header=FBSDisplayConfiguration.h];
 * wire keys [guess]. */
#define FB_DISP_PIXEL_SIZE "pixelSize"
#define FB_DISP_POINT_SCALE "pointScale"
#define FB_DISP_BOUNDS "bounds"
#define FB_DISP_NATIVE_ORIENTATION "nativeOrientation"
#define FB_DISP_RENDERING_CENTER "renderingCenter"
#define FB_DISP_MAIN_DISPLAY "mainDisplay"
#define FB_DISP_CURRENT_MODE "currentMode"
#define FB_DISP_PREFERRED_MODE "preferredMode"
#define FB_DISP_REFRESH_RATE "refreshRate"
#define FB_DISP_COLOR_GAMUT "colorGamut"

/* ------------------------------------------------------------------ */
/* Display values for 1440x3200 @ scale 4.                            */
/* Measured BKS reply: 1440.0 x 3200.0 px, scale 4.0, rotation 0.0.    */
/* Point size is pixels / scale, matching GSMainScreenPointSize.      */
/* The three screen sources, BKS, MobileGestalt and FrontBoard, must  */
/* agree, or the window scale depends on which one a caller reads     */
/* first.                                                             */
/* ------------------------------------------------------------------ */

#define FB_DISPLAY_WIDTH_PX 1440.0
#define FB_DISPLAY_HEIGHT_PX 3200.0
#define FB_DISPLAY_SCALE 4.0
#define FB_DISPLAY_WIDTH_PT 360.0
#define FB_DISPLAY_HEIGHT_PT 800.0
#define FB_DISPLAY_ROTATION 0.0

/* Interface orientation values. portrait (1) is from a real settings
 * description dump
 * [src: https://github.com/mikehouse/Apple-TV-Player/issues/4]
 * ("interfaceOrientation = portrait (1)"). */
#define FB_ORIENTATION_PORTRAIT 1

/* Settings values
 * [src: https://github.com/khanhduytran0/FrontBoardAppLauncher]
 * (UIMutableApplicationSceneSettings as set by the reference host). */
#define FB_SETTINGS_LEVEL 1.0
#define FB_SETTINGS_STATUS_BAR_STYLE 0

/* ------------------------------------------------------------------ */
/* Verified BSXPC / BaseBoard / FrontBoard wire strings & constants    */
/* (iOS 26.6.2 dsc).                                                  */
/* ------------------------------------------------------------------ */

/* Positional argument keys [src: 0x1e6957c60]. */
#define FB_BSXPC_ARG_1 "1"
#define FB_BSXPC_ARG_2 "2"
#define FB_BSXPC_ARG_3 "3"

/* BSXPCCoder & secure-coding wrapper keys [src: 0x1efb46578, 0x1efb46558]. */
#define FB_BSX_KEY_CLASS "bsx_class"
#define FB_BSX_KEY_SEC_CLASS "__BSXPCSecureClass"
#define FB_BSX_KEY_SEC_VALUE "__BSXPCSecureValue"
#define FB_BSX_CODER_TYPE_SECURE 1LL

/* ObjC class names instantiated by BSXPCCoder. */
#define FB_CLS_SCENE_IDENTITY "FBSSceneIdentity"
#define FB_CLS_SCENE_PARAMETERS "FBSSceneParameters"
#define FB_CLS_SCENE_SETTINGS_DIFF "FBSSceneSettingsDiff"
#define FB_CLS_UI_SCENE_SETTINGS "UIApplicationSceneSettings"
#define FB_CLS_KEYED_SETTINGS_DIFF "BSKeyedSettingsDiff"
#define FB_CLS_KEYED_SETTINGS "BSKeyedSettings"
#define FB_CLS_SETTINGS_DIFF "BSSettingsDiff"
#define FB_CLS_SETTINGS "BSSettings"
#define FB_CLS_TRANSITION_CONTEXT "UIApplicationSceneTransitionContext"
#define FB_CLS_NSVALUE "NSValue"
#define FB_CLS_NSNUMBER "NSNumber"
#define FB_CLS_DISPLAY_CONFIG "FBSDisplayConfiguration"
#define FB_CLS_DISPLAY_IDENTITY "FBSDisplayIdentity"
#define FB_CLS_DISPLAY_MODE "FBSDisplayMode"

/* FBSSceneIdentity keys [src: 0x1f0d95ad8, 0x1f0d96dd8, 0x1f0d9a458]. */
#define FB_ID_KEY_I "i"
#define FB_ID_KEY_W "w"
#define FB_ID_KEY_IW "iw"
#define FB_DEFAULT_WORKSPACE_NAME "default"

/* FBSSettingsDiff & BSSettings / BSKeyedSettings keys
 * [src: 0x1f0d975d8, 0x1f0d975f8, 0x1f0d97618, 0x1906df0ec, 0x190704adc]. */
#define FB_DIFF_MAIN "mainDiff"
#define FB_DIFF_OTHER "otherDiff"
#define FB_DIFF_SETTINGS_CLASS "settingsClass"
#define FB_DIFF_CHANGES "_changes"
#define FB_SETTINGS_FLAGS "flags"
#define FB_SETTINGS_OBJECTS "objects"
#define FB_SETTINGS_BSKEYS "BSKeys"

/* NSValue(BSCoding) keys & CGRect objCType [src: 0x1efb3ee18, 0x1efb3f2b8]. */
#define FB_NSVALUE_DATA "data"
#define FB_NSVALUE_TYPE "type"
#define FB_OBJC_TYPE_CGRECT "{CGRect={CGPoint=dd}{CGSize=dd}}"

/* Exact ___CFStringHash values for FBSSceneSettingsCore keys
 * [src: 0x1834fb720, 0x1a475a7d4]. */
#define FB_CORE_HASH_FOREGROUND 0x2a71e1c084937645ULL
#define FB_CORE_HASH_FRAME 0x000010044a783a10ULL
#define FB_CORE_HASH_DISPLAY_CONFIG 0xed9405554827cd62ULL
#define FB_CORE_HASH_LEVEL 0x000010cbb7ccaebdULL
#define FB_CORE_HASH_INTERFACE_ORIENTATION 0x6e51b4d92a906951ULL

/* UIApplicationSceneSettings legacy setting IDs [src: 0x1e6515be8]. */
#define FB_UI_SET_UNDER_LOCK 1ULL
#define FB_UI_SET_DEACTIVATION_REASONS 3ULL
#define FB_UI_SET_DEV_ORIENT_EVENTS 6ULL
#define FB_UI_SET_STATUS_BAR_DISABLED 9ULL
#define FB_UI_SET_USER_INTERFACE_STYLE 10ULL
#define FB_UI_SET_DEVICE_ORIENTATION 13ULL
#define FB_UI_SET_INTERFACE_ORIENT_MODE 35ULL
#define FB_UI_SET_ENHANCED_WINDOWING 40ULL

/* FBSDisplayConfiguration / FBSDisplayIdentity / FBSDisplayMode keys
 * [src: 0x1f0d94d38..0x1f0d9bad8]. */
#define FB_DISP_IDENTITY "identity"
#define FB_DISP_NATIVE_BOUNDS "nativeBounds"
#define FB_DISP_SAFE_OVERSCAN_RATIO "safeOverscanRatio"
#define FB_DISP_LATENCY "latency"
#define FB_DISP_HARDWARE_ID "hardwareIdentifier"
#define FB_DISP_NAME "name"
#define FB_DISP_DEVICE_NAME "deviceName"
#define FB_DISP_SEED "seed"
#define FB_DISP_NOT_COMPARABLE "notComparable"
#define FB_DISP_TAGS "tags"
#define FB_DISP_OTHER_MODES "otherModes"
#define FB_DISP_CLONING_SUPPORTED "cloningSupported"
#define FB_DISP_OVERSCANNED "overscanned"
#define FB_DISP_OVERSCAN_COMP "overscanCompensation"

#define FB_DISPID_TYPE "type"
#define FB_DISPID_CONN_TYPE "connectionType"
#define FB_DISPID_MAIN_LIKE "mainLike"
#define FB_DISPID_DISPLAY_ID "displayID"
#define FB_DISPID_CONN_SEED "connectionSeed"
#define FB_DISPID_PID "pid"
#define FB_DISPID_EXTERNAL "external"
#define FB_DISPID_UNIQUE_ID "uniqueID"
#define FB_DISPID_SECURE "secure"

#define FB_DISPMODE_WIDTH "width"
#define FB_DISPMODE_HEIGHT "height"
#define FB_DISPMODE_PREF_SCALE "preferredScale"
#define FB_DISPMODE_SCALE_OVERRIDE "scaleOverride"
#define FB_DISPMODE_GAMUT "gamut"
#define FB_DISPMODE_HDR "hdr"
#define FB_DISPMODE_ROTATION "rotation"
#define FB_DISPMODE_VIRTUAL "virtual"

/* Display identity string constants. */
#define FB_DISPLAY_HW_ID "MainDisplay"
#define FB_DISPLAY_NAME "LCD"

/* Trace messages (kept in header so fakeboard.c stays free of literals). */
#define FB_TRACE_WS_EP "[fakeboard] workspace endpoint set\n"
#define FB_TRACE_NO_EP "[fakeboard] callout queued, no endpoint\n"
#define FB_TRACE_WS_CONN_PRE "[fakeboard] workspace connect: scene create rc="

/* ------------------------------------------------------------------ */
/* Client-endpoint discovery. The keys a handshake message may carry  */
/* a mach_send port under. All [guess] (frontboard-scene.md           */
/* section 10: the handshake wire format is unverified).      */
/* ------------------------------------------------------------------ */

#define FB_KEY_ENDPOINT "endpoint"
#define FB_KEY_CONNECTION "connection"
#define FB_KEY_CLIENT_ENDPOINT "clientEndpoint"
#define FB_KEY_CLIENT_PORT "clientPort"

/* ------------------------------------------------------------------ */
/* RunningBoard (fakerb.c). All [guess] unless noted. The            */
/* RBSConnection method names are ObjC selectors from RBSConnection.h */
/* [src: https://developer.limneos.net/?ios=18.1&framework=RunningBoardServices.framework&header=RBSConnection.h]; */
/* their use as wire message names is [guess]. RBSProcessState       */
/* property names are from RBSProcessState.h (same portal); their    */
/* use as wire keys is [guess].                                      */
/* ------------------------------------------------------------------ */

/* Mach service name, as looked up through bootstrap_look_up. The
 * RBSConnection class is sourced; the Mach service name string is
 * not verified [guess]. */
#define FAKERB_SERVICE_NAME "com.apple.runningboard"

/* Reply keys. */
#define FAKERB_KEY_HANDLE     "handle"
#define FAKERB_KEY_STATES     "states"
#define FAKERB_KEY_STATE      "state"
#define FAKERB_KEY_IDENTIFIER "identifier"

/* Message names (full ObjC selector spellings, as a BSXPC service
 * connection encodes them). [guess] Every one is a candidate for
 * correction after verification; the dispatch table in fakerb.c
 * matches on these strings only. The handshake selector is measured,
 * not guessed: it reaches fakerb on device (2026-11-12). */
#define FAKERB_MSG_HANDSHAKE "handshakeWithRequest:"
#define FAKERB_MSG_HANDLE_FOR_PREDICATE    "handleForPredicate:error:"
#define FAKERB_MSG_HANDLE_FOR_PID          "handleForProcessIdentifier:error:"
#define FAKERB_MSG_STATES_FOR_PREDICATE    "statesForPredicate:withDescriptor:error:"
#define FAKERB_MSG_ACQUIRE_ASSERTION       "acquireAssertion:error:"
#define FAKERB_MSG_INVALIDATE_ASSERTION    "invalidateAssertion:error:"
#define FAKERB_MSG_INVALIDATE_ASSERTION_ID "invalidateAssertionWithIdentifier:error:"
#define FAKERB_MSG_SUBSCRIBE_STATE         "subscribeProcessStateMonitor:configuration:error:"
#define FAKERB_MSG_UNSUBSCRIBE_STATE       "unsubscribeProcessStateMonitorWithIdentifier:error:"
#define FAKERB_MSG_EXECUTE_LAUNCH          "executeLaunchRequest:"
#define FAKERB_MSG_EXECUTE_TERMINATE       "executeTerminateRequest:assertion:error:"
#define FAKERB_MSG_SUBSCRIBE_DEATH         "subscribeToProcessDeath:handler:"
#define FAKERB_MSG_SAVE_ENDOWMENT          "saveEndowment:withError:"

/* handshakeWithRequest: argument keys. The selector reaches fakerb
 * on device (2026-11-12). The
 * key spellings are the handshake parameter names
 * (RBSHandshakeRequest, _assertionDescriptors, _savedEndowments);
 * their use as wire keys is [guess]. v1 accepts the arguments
 * without validating them (see rb_handshake in fakerb.c); if a later
 * change validates them, read them here with rb_find_arg. */
#define FAKERB_ARG_HANDSHAKE_REQUEST "RBSHandshakeRequest"
#define FAKERB_ARG_ASSERTION_DESCRIPTORS "_assertionDescriptors"
#define FAKERB_ARG_SAVED_ENDOWMENTS "_savedEndowments"

/* RBSProcessState property names, used as the wire keys of the state
 * dict. Key spellings [guess]. */
#define FAKERB_STATE_KEY_TASK_STATE         "taskState"
#define FAKERB_STATE_KEY_IS_FOREGROUND      "isForeground"
#define FAKERB_STATE_KEY_IS_RUNNING         "isRunning"
#define FAKERB_STATE_KEY_IS_SUSPENDED       "isSuspended"
#define FAKERB_STATE_KEY_IS_TERMINATING     "isTerminating"
#define FAKERB_STATE_KEY_LAST_EXIT_STATUS   "lastExitStatus"
#define FAKERB_STATE_KEY_TERMINATION_REASON "terminationReason"
#define FAKERB_STATE_KEY_ENDOWMENTS         "endowmentNamespaces"

/* The visibility endowment namespace the WebKit foreground check
 * looks for (sourced from the WebKit ApplicationStateTracker change). */
#define FAKERB_ENDOWMENT_VISIBILITY "com.apple.frontboard.visibility"

/* RBSTaskState value reported for the app process. [guess] The enum
 * lives in RunningBoardServices-Structs.h, which was not read; 1 is
 * a placeholder for "running scheduled" until verification measures
 * the real value. */
#define FAKERB_TASK_STATE_RUNNING_SCHEDULED 1U

/* Default bundle id used until fakerb_set_self_bundle_id overrides
 * it. [guess] that the real bundle id is Aidoku's; the host
 * simulator overrides it. */
#define FAKERB_DEFAULT_BUNDLE_ID "com.example.Aidoku"

/* ------------------------------------------------------------------ */
/* Non-wire formatting helpers (not sent on the wire, so they need no  */
/* source tag, but they live here so fakeboard.c keeps zero string     */
/* literals).                                                          */
/* ------------------------------------------------------------------ */

/* Log prefix for bsxpc_log_dict. */
#define FB_LOG_PREFIX "[fakeboard]"
/* Log prefix for the bsxpc_log_dict calls in fakerb.c. */
#define FAKERB_LOG_PREFIX "[fakerb]"
/* Fallback bundle id when no bundle id can be learned from the
 * client or from fakeboard_scene_create. */
#define FB_DEFAULT_BUNDLE_ID "com.ioswine.unknown"
/* Persistence-identifier UUID format [guess]: a fixed prefix plus a
 * counter in the first 8 hex digits, e.g.
 * "ioswine-00000001-0000-4000-8000-000000000000". Real devices use a
 * fresh NSUUID per scene
 * [src: https://github.com/khanhduytran0/FrontBoardAppLauncher];
 * the format of the string itself is our choice. */
#define FB_UUID_PREFIX "ioswine-"
#define FB_UUID_TAIL "-0000-4000-8000-000000000000"
#define FB_UUID_HEX_DIGITS "0123456789abcdef"
