#include <gst/gst.h>
#include <unistd.h>
#include "gst_mtl_cpu_element.h"
#define pthread (100)

GST_DEBUG_CATEGORY_STATIC(gst_mtl_cpu_element_debug);
#define GST_CAT_DEFAULT gst_mtl_cpu_element_debug

#define GST_LICENSE "LGPL"
#define GST_API_VERSION "1.0"
#define GST_PACKAGE_NAME "Media Transport Library SMPTE ST 2110-40 Tx plugin"
#define GST_PACKAGE_ORIGIN "https://github.com/OpenVisualCloud/Media-Transport-Library"
#define PACKAGE "gst-mtl-cpu-element"
#define PACKAGE_VERSION "1.0"

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

    while (*(volatile int*)arg) {  // Use volatile to prevent optimization
        for (guint i = 0; i < __UINT32_MAX__; i++) {
            calc++;
            for (guint j = 0; j < __UINT32_MAX__; j++) {
                calc += j * j;
            }
            for (guint j = 0; j < __UINT32_MAX__; j++) {
                calc += j ^ (j - 1);
            }

            if (calc == 2831) usleep(1000);
        }
    }
    return NULL;
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