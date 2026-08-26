#define PLUG_NAME "Sparkles"
#define PLUG_MFR "JETIstudios"
#define PLUG_VERSION_HEX 0x00000000
#define PLUG_VERSION_STR "1.0.0"
#define PLUG_UNIQUE_ID '3FyK'
#define PLUG_MFR_ID 'Jsts'
#define PLUG_URL_STR "https://www.youtube.com/@jetistudios7153"
#define PLUG_EMAIL_STR "jetistudios@gmail.com"
#define PLUG_COPYRIGHT_STR "Copyright 2025 JETI Studios"
#define PLUG_CLASS_NAME Sparkles

#define BUNDLE_NAME "Sparkles"
#define BUNDLE_MFR "JETIstudios"
#define BUNDLE_DOMAIN "com"

#define SHARED_RESOURCES_SUBPATH "Sparkles"

#define PLUG_CHANNEL_IO "2-2"

#define PLUG_LATENCY 0
#define PLUG_TYPE 0
#define PLUG_DOES_MIDI_IN 1
#define PLUG_DOES_MIDI_OUT 1
#define PLUG_DOES_MPE 0
#define PLUG_DOES_STATE_CHUNKS 0
#define PLUG_HAS_UI 1
// Half of resources/img/background.png's own pixel size (1497x828), so the default view matches
// that artwork's aspect ratio exactly. PLUG_HOST_RESIZE is 0 and MIN/MAX equal PLUG_WIDTH/HEIGHT --
// together these mean no host ever resizes this logical size directly, so the only way to resize
// at all is via the in-UI corner handle, which (see EUIResizerMode::Scale in mLayoutFunc) zooms
// the whole canvas instead of relaying out at a new width/height. That's what keeps the aspect
// ratio locked and makes every knob/label/font scale with the window for free -- no aspect-lock
// math of our own needed. See CLAUDE.md's background-artwork section.
#define PLUG_WIDTH 748
#define PLUG_HEIGHT 414
#define PLUG_FPS 60
#define PLUG_SHARED_RESOURCES 0
#define PLUG_HOST_RESIZE 0
#define PLUG_MIN_WIDTH PLUG_WIDTH
#define PLUG_MIN_HEIGHT PLUG_HEIGHT
#define PLUG_MAX_WIDTH PLUG_WIDTH
#define PLUG_MAX_HEIGHT PLUG_HEIGHT

#define AUV2_ENTRY Sparkles_Entry
#define AUV2_ENTRY_STR "Sparkles_Entry"
#define AUV2_FACTORY Sparkles_Factory
#define AUV2_VIEW_CLASS Sparkles_View
#define AUV2_VIEW_CLASS_STR "Sparkles_View"

#define AAX_TYPE_IDS 'ITP1'
#define AAX_TYPE_IDS_AUDIOSUITE 'ITA1'
#define AAX_PLUG_MFR_STR "Jsts"
#define AAX_PLUG_NAME_STR "Sparkles\nIPEF"
#define AAX_PLUG_CATEGORY_STR "Effect"
#define AAX_DOES_AUDIOSUITE 0

#define VST3_SUBCATEGORY "Fx"

#define CLAP_MANUAL_URL "https://iplug2.github.io/manuals/example_manual.pdf"
#define CLAP_SUPPORT_URL "https://github.com/iPlug2/iPlug2/wiki"
#define CLAP_DESCRIPTION "Analyzes audio input and generates MIDI output"
#define CLAP_FEATURES "audio-effect"//, "utility"

#define APP_NUM_CHANNELS 2
#define APP_N_VECTOR_WAIT 0
#define APP_MULT 1
#define APP_COPY_AUV3 0
#define APP_SIGNAL_VECTOR_SIZE 64

#define FREDOKA_LIGHT_FN "Fredoka-Light.ttf"
#define FREDOKA_REGULAR_FN "Fredoka-Regular.ttf"
#define FREDOKA_MEDIUM_FN "Fredoka-Medium.ttf"
#define FREDOKA_SEMIBOLD_FN "Fredoka-SemiBold.ttf"
#define FREDOKA_BOLD_FN "Fredoka-Bold.ttf"

#define BACKGROUND_FN "background.png"
#define QUICK_GUIDE_FN "quick_guide.png"
