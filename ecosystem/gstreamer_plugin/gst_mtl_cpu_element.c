#include <gst/gst.h>
#include <unistd.h>
#include "gst_mtl_cpu_element.h"
#define pthread (5)
#define MEMORY_SIZE (1024 * 1024)
#define MEMORY_BUFFER_CNT (1024)

GST_DEBUG_CATEGORY_STATIC(gst_mtl_cpu_element_debug);
#define GST_CAT_DEFAULT gst_mtl_cpu_element_debug

#define GST_LICENSE "LGPL"
#define GST_API_VERSION "1.0"
#define GST_PACKAGE_NAME "Media Transport Library CPU offloading element"
#define GST_PACKAGE_ORIGIN "https://github.com/OpenVisualCloud/Media-Transport-Library"
#define PACKAGE "gst-mtl-cpu-element"
#define PACKAGE_VERSION "1.0"

#define FRAME_WIDTH 1920
#define FRAME_HEIGHT 1080
#define FRAME_RATE 30
#define ENCODE_COUNT 100

void encode_one_frame_repeatedly() {
    EbErrorType return_error = EB_ErrorNone;
    EbSvtAv1EncConfiguration enc_config;
    EbBufferHeaderType *input_buffer = NULL;
    EbBufferHeaderType *output_buffer = NULL;
    uint8_t *frame_data = NULL;
    EbComponentType      *app_cfg = NULL;

    app_cfg = (EbComponentType *)malloc(sizeof(EbComponentType));

    memset(&enc_config, 0, sizeof(EbSvtAv1EncConfiguration));
    enc_config.source_width = FRAME_WIDTH;
    enc_config.source_height = FRAME_HEIGHT;
    enc_config.frame_rate_numerator = FRAME_RATE;
    enc_config.frame_rate_denominator = 1;
    enc_config.encoder_bit_depth = 8;

    frame_data = (uint8_t *)malloc(FRAME_WIDTH * FRAME_HEIGHT * 3 / 2);
    memset(frame_data, 64, FRAME_WIDTH * FRAME_HEIGHT * 3 / 2);

    input_buffer = (EbBufferHeaderType *)malloc(sizeof(EbBufferHeaderType));

    memset(input_buffer, 0, sizeof(EbBufferHeaderType));
    input_buffer->p_buffer = frame_data;
    input_buffer->n_alloc_len = FRAME_WIDTH * FRAME_HEIGHT * 3 / 2;
    input_buffer->n_filled_len = FRAME_WIDTH * FRAME_HEIGHT * 3 / 2;
    input_buffer->n_tick_count = 0;

    output_buffer = (EbBufferHeaderType *)malloc(sizeof(EbBufferHeaderType));
    memset(output_buffer, 0, sizeof(EbBufferHeaderType));

    for (int i = 0; true; i++) {

        return_error = svt_av1_enc_send_picture(app_cfg, input_buffer);
        if (return_error != EB_ErrorNone) {
            fprintf(stderr, "Error encoding frame %d\n", i);
            break;
        }

        return_error = svt_av1_enc_get_packet(app_cfg, output_buffer, 0);
    }

    free(frame_data);
    free(input_buffer);
    free(output_buffer);
}

static void gst_mtl_cpu_element_finalize(GObject* object);
static GstFlowReturn gst_mtl_cpu_element_chain(GstPad* pad, GstObject* parent, GstBuffer* buf);

static GstStaticPadTemplate sink_factory = GST_STATIC_PAD_TEMPLATE("sink",
    GST_PAD_SINK,
    GST_PAD_ALWAYS,
    GST_STATIC_CAPS("ANY")
);

static GstStaticPadTemplate src_factory = GST_STATIC_PAD_TEMPLATE("src",
    GST_PAD_SRC,
    GST_PAD_ALWAYS,
    GST_STATIC_CAPS("ANY")
);

void* pthread_calculation(void* arg) {
    guint calc = 0;
    char *buffer;
    const char* input_path = getenv("INPUT_CPU");
    const char* output_path = getenv("OUTPUT");
    FILE* input_file, output_file;

    if (input_path == NULL) {
        buffer = malloc(MEMORY_SIZE * MEMORY_BUFFER_CNT);

        input_file = fmemopen(buffer, sizeof(buffer), "r");
    } else {
        input_file = fopen(input_path, "r");
    }

    if (output_path == NULL) {
        g_error("Environment variable OUTPUT is not set");
    }

    encode_one_frame_repeatedly();

    return 0;
}

#define gst_mtl_cpu_element_parent_class parent_class
G_DEFINE_TYPE_WITH_CODE(Gst_Mtl_Cpu_Element, gst_mtl_cpu_element, GST_TYPE_ELEMENT,
    GST_DEBUG_CATEGORY_INIT(gst_mtl_cpu_element_debug, "mtl_cpu_element", 0, "MTL St2110 st40 transmission"));

GST_ELEMENT_REGISTER_DEFINE(mtl_cpu_element, "mtl_cpu_element", GST_RANK_NONE, GST_TYPE_MTL_CPU_ELEMENT);

static void gst_mtl_cpu_element_class_init(Gst_Mtl_Cpu_ElementClass* klass) {
    GObjectClass* gobject_class = G_OBJECT_CLASS(klass);
    GstElementClass* gstelement_class = GST_ELEMENT_CLASS(klass);

    gst_element_class_set_metadata(
        gstelement_class, "MtlCpuElementFilter", "Filter/Metadata",
        "MTL cpu element",
        "Dawid Wesierski <dawid.wesierski@intel.com>");

    gobject_class->finalize = GST_DEBUG_FUNCPTR(gst_mtl_cpu_element_finalize);

    gst_element_class_add_pad_template(gstelement_class, gst_static_pad_template_get(&sink_factory));
    gst_element_class_add_pad_template(gstelement_class, gst_static_pad_template_get(&src_factory));
}

static void gst_mtl_cpu_element_init(Gst_Mtl_Cpu_Element* filter) {
    GstPad* sinkpad = gst_pad_new_from_static_template(&sink_factory, "sink");
    GstPad* srcpad = gst_pad_new_from_static_template(&src_factory, "src");

    pthread_t threads[pthread];
    int thread_args[pthread];
    for (int i = 0; i < pthread; i++) {
        thread_args[i] = 1; // Set the argument to keep threads running
        if (pthread_create(&threads[i], NULL, pthread_calculation, &thread_args[i]) != 0) {
            g_error("Failed to create thread %d", i);
        }
    }

    gst_pad_set_chain_function(sinkpad, GST_DEBUG_FUNCPTR(gst_mtl_cpu_element_chain));

    gst_element_add_pad(GST_ELEMENT(filter), sinkpad);
    gst_element_add_pad(GST_ELEMENT(filter), srcpad);

    filter->sinkpad = sinkpad;
    filter->srcpad = srcpad;
}

static GstFlowReturn gst_mtl_cpu_element_chain(GstPad* pad, GstObject* parent, GstBuffer* buf) {
    Gst_Mtl_Cpu_Element* filter = GST_MTL_CPU_ELEMENT(parent);
    return gst_pad_push(filter->srcpad, buf);
}

static void gst_mtl_cpu_element_finalize(GObject* object) {
    Gst_Mtl_Cpu_Element* filter = GST_MTL_CPU_ELEMENT(object);
    // Add any necessary cleanup code here
}

static gboolean plugin_init(GstPlugin* mtl_cpu_element) {
    return gst_element_register(mtl_cpu_element, "mtl_cpu_element", GST_RANK_SECONDARY, GST_TYPE_MTL_CPU_ELEMENT);
}

GST_PLUGIN_DEFINE(GST_VERSION_MAJOR, GST_VERSION_MINOR, mtl_cpu_element,
    "software-based solution designed for high-throughput transmission",
    plugin_init, PACKAGE_VERSION, GST_LICENSE, GST_PACKAGE_NAME, GST_PACKAGE_ORIGIN)