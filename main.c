#include <gtk/gtk.h>
#include <gdk/gdkx.h>

#include <gst/gst.h>
#include <gst/gl/gl.h>

#include <GL/gl.h>
#include <GL/glx.h>

#if GST_GL_HAVE_WINDOW_X11 && defined (GDK_WINDOWING_X11)
#include <X11/Xlib.h>
#include <gst/gl/x11/gstgldisplay_x11.h>
#endif

#define GLAREA_ERROR (glarea_error_quark ())

typedef enum {
  GLAREA_ERROR_SHADER_COMPILATION,
  GLAREA_ERROR_SHADER_LINK
} GlareaError;

G_DEFINE_QUARK (glarea-error, glarea_error)

static char const *vertex_shader_str =
"attribute vec3 aVertexPosition;\n"
"attribute vec2 aTextureCoord;\n"
"varying vec2 vTextureCoord;\n"
"void main(void) {\n"
"  gl_Position = vec4(aVertexPosition, 1.0);\n"
"  vTextureCoord = aTextureCoord;\n"
"}\n";

static char const *fragment_shader_str =
"varying vec2 vTextureCoord;\n"
"uniform sampler2D uSampler;\n"
"void main(void) {\n"
"  gl_FragColor = texture2D(uSampler, vec2(vTextureCoord.s, vTextureCoord.t));\n"
"}\n";

struct vertex_info {
  float position[3];
  float texture_coord[2];
};

static const struct vertex_info vertex_data[] = {
  { { -1.0f, -1.0f,  1.0f }, { 0.0f, 0.0f } },
  { {  1.0f, -1.0f,  1.0f }, { 1.0f, 0.0f } },
  { {  1.0f,  1.0f,  1.0f }, { 1.0f, 1.0f } },
  { { -1.0f,  1.0f,  1.0f }, { 0.0f, 1.0f } },
};

static int const vertex_indice[] = {
 0, 1, 2, 0, 2, 3,
};

static struct {
  GLXContext gl_context;
  Display *display;
  GstGLContext *gst_gl_context;
  GstGLDisplay *gst_gl_display;

  GtkWidget *gl_area;
  GstVideoFrame *video_frame;
  guint texture;
  guint vertex_buffer;
  guint indice_buffer;
  guint program;
  guint vertex_pos_attrib;
  guint texture_coord_attrib;
  guint texture_attrib;
} scene_info;

static void
init_buffers (guint  vertex_pos_attrib,
              guint  texture_coord_attrib,
              guint *vertex_buffer_out,
              guint *indice_buffer_out)
{
  guint vertex_buffer, indice_buffer;

  glGenBuffers (1, &vertex_buffer);
  glBindBuffer (GL_ARRAY_BUFFER, vertex_buffer);
  glBufferData (GL_ARRAY_BUFFER, sizeof (vertex_data), vertex_data, GL_STATIC_DRAW);

  /* enable and set the position attribute */
  glEnableVertexAttribArray (vertex_pos_attrib);
  glVertexAttribPointer (vertex_pos_attrib, 4, GL_FLOAT, GL_FALSE,
                         sizeof (struct vertex_info),
                         (GLvoid *) (G_STRUCT_OFFSET (struct vertex_info, position)));

  /* enable and set the color attribute */
  glEnableVertexAttribArray (texture_coord_attrib);
  glVertexAttribPointer (texture_coord_attrib, 4, GL_FLOAT, GL_FALSE,
                         sizeof (struct vertex_info),
                         (GLvoid *) (G_STRUCT_OFFSET (struct vertex_info, texture_coord)));

  glGenBuffers (1, &indice_buffer);
  glBindBuffer (GL_ELEMENT_ARRAY_BUFFER, indice_buffer);
  glBufferData (GL_ELEMENT_ARRAY_BUFFER, sizeof (vertex_indice), vertex_indice, GL_STATIC_DRAW);

  /* reset the state; we will re-enable buffers when needed */
  glBindBuffer (GL_ARRAY_BUFFER, 0);

  if (vertex_buffer_out != NULL)
    *vertex_buffer_out = vertex_buffer;
  if (indice_buffer_out != NULL)
    *indice_buffer_out = indice_buffer;
}

static guint
create_shader (int          shader_type,
               const char  *source,
               GError     **error,
               guint       *shader_out)
{
  guint shader = glCreateShader (shader_type);
  glShaderSource (shader, 1, &source, NULL);
  glCompileShader (shader);

  int status;
  glGetShaderiv (shader, GL_COMPILE_STATUS, &status);
  if (status == GL_FALSE)
    {
      int log_len;
      glGetShaderiv (shader, GL_INFO_LOG_LENGTH, &log_len);

      char *buffer = g_malloc (log_len + 1);
      glGetShaderInfoLog (shader, log_len, NULL, buffer);

      g_set_error (error, GLAREA_ERROR, GLAREA_ERROR_SHADER_COMPILATION,
                   "Compilation failure in %s shader: %s",
                   shader_type == GL_VERTEX_SHADER ? "vertex" : "fragment",
                   buffer);

      g_free (buffer);

      glDeleteShader (shader);
      shader = 0;
    }

  if (shader_out != NULL)
    *shader_out = shader;

  return shader != 0;
}

static gboolean
init_shaders (guint   *program_out,
              guint   *vertex_pos_attrib_out,
              guint   *texture_coord_attrib_out,
              guint   *texture_attrib_out,
              GError **error)
{
  guint program = 0;
  guint vertex_pos_attrib = 0;
  guint texture_coord_attrib = 0;
  guint texture_attrib = 0;
  guint vertex = 0, fragment = 0;

  /* load the vertex shader */
  create_shader (GL_VERTEX_SHADER, vertex_shader_str, error, &vertex);
  if (vertex == 0)
    goto out;

  /* load the fragment shader */
  create_shader (GL_FRAGMENT_SHADER, fragment_shader_str, error, &fragment);
  if (fragment == 0)
    goto out;

  /* link the vertex and fragment shaders together */
  program = glCreateProgram ();
  glAttachShader (program, vertex);
  glAttachShader (program, fragment);
  glLinkProgram (program);

  int status = 0;
  glGetProgramiv (program, GL_LINK_STATUS, &status);
  if (status == GL_FALSE)
    {
      int log_len = 0;
      glGetProgramiv (program, GL_INFO_LOG_LENGTH, &log_len);

      char *buffer = g_malloc (log_len + 1);
      glGetProgramInfoLog (program, log_len, NULL, buffer);

      g_set_error (error, GLAREA_ERROR, GLAREA_ERROR_SHADER_LINK,
                   "Linking failure in program: %s", buffer);

      g_free (buffer);

      glDeleteProgram (program);
      program = 0;

      goto out;
    }

  vertex_pos_attrib = glGetUniformLocation (program, "aVertexPosition");
  texture_coord_attrib = glGetAttribLocation (program, "aTextureCoord");
  texture_attrib = glGetUniformLocation (program, "uSampler");

  /* the individual shaders can be detached and destroyed */
  glDetachShader (program, vertex);
  glDetachShader (program, fragment);

out:
  if (vertex != 0)
    glDeleteShader (vertex);
  if (fragment != 0)
    glDeleteShader (fragment);

  if (program_out != NULL)
    *program_out = program;
  if (vertex_pos_attrib_out != NULL)
    *vertex_pos_attrib_out = vertex_pos_attrib;
  if (texture_coord_attrib_out != NULL)
    *texture_coord_attrib_out = texture_coord_attrib;
  if (texture_attrib_out != NULL)
    *texture_attrib_out = texture_attrib;

  return program != 0;
}

static void
realize (GtkWidget *widget)
{
  /* we need to ensure that the GdkGLContext is set before calling GL API */
  gtk_gl_area_make_current (GTK_GL_AREA (widget));

  /* if the GtkGLArea is in an error state we don't do anything */
  if (gtk_gl_area_get_error (GTK_GL_AREA (widget)) != NULL)
    return;

  /* initialize the shaders and retrieve the program data */
  GError *error = NULL;
  if (!init_shaders (&scene_info.program,
                     &scene_info.vertex_pos_attrib,
                     &scene_info.texture_coord_attrib,
                     &scene_info.texture_attrib,
                     &error))
    {
      /* set the GtkGLArea in error state, so we'll see the error message
       * rendered inside the viewport
       */
      gtk_gl_area_set_error (GTK_GL_AREA (widget), error);
      g_error_free (error);
      return;
    }

  /* initialize the vertex buffers */
  init_buffers (scene_info.vertex_pos_attrib, scene_info.texture_coord_attrib,
                &scene_info.vertex_buffer, &scene_info.indice_buffer);

  scene_info.gl_context = glXGetCurrentContext();
  scene_info.display = gdk_x11_display_get_xdisplay (gdk_display_get_default ());
}

static void
unrealize (GtkWidget *widget)
{
  /* we need to ensure that the GdkGLContext is set before calling GL API */
  gtk_gl_area_make_current (GTK_GL_AREA (widget));

  /* skip everything if we're in error state */
  if (gtk_gl_area_get_error (GTK_GL_AREA (widget)) != NULL)
    return;

  /* destroy all the resources we created */
  if (scene_info.vertex_buffer != 0)
    glDeleteBuffers (1, &scene_info.vertex_buffer);
  if (scene_info.indice_buffer != 0)
    glDeleteBuffers (1, &scene_info.indice_buffer);
  if (scene_info.program != 0)
    glDeleteProgram (scene_info.program);
}

static gboolean
render (GtkGLArea *area, GdkGLContext *context)
{
  // inside this function it's safe to use GL; the given
  // #GdkGLContext has been made current to the drawable
  // surface used by the #GtkGLArea and the viewport has
  // already been set to be the size of the allocation

  // we can start by clearing the buffer
  glClearColor (0, 0, 0, 0);
  glClear (GL_COLOR_BUFFER_BIT);

  if (scene_info.program == 0 || scene_info.vertex_buffer == 0)
    return TRUE;

  /* load our program */
  glUseProgram (scene_info.program);

  glBindBuffer(GL_ARRAY_BUFFER, scene_info.vertex_buffer);
  glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, scene_info.indice_buffer);

  glActiveTexture(GL_TEXTURE0);
  glBindTexture(GL_TEXTURE_2D, scene_info.texture);
  glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
  glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
  glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
  glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

  glUniform1i (scene_info.texture_attrib, 0);

  /* draw the three vertices as a triangle */
  glDrawElements (GL_TRIANGLES, 6, GL_UNSIGNED_SHORT, 0);

  /* we finished using the buffers and program */
  glBindBuffer(GL_ARRAY_BUFFER, 0);
  glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, 0);
  glUseProgram (0);

  // we completed our drawing; the draw commands will be
  // flushed at the end of the signal emission chain, and
  // the buffers will be drawn on the window
  return TRUE;
}

//client draw callback
static gboolean drawCallback (GstElement *gl_sink, GstGLContext *context, GstSample *sample, gpointer data)
{
  GstBuffer *buf = gst_sample_get_buffer (sample);
  GstCaps *caps = gst_sample_get_caps (sample);
  GstVideoInfo video_info;

  gst_video_info_from_caps (&video_info, caps);

  if (scene_info.video_frame) {
    gst_video_frame_unmap (scene_info.video_frame);
    g_free (scene_info.video_frame);
  }

  scene_info.video_frame = g_malloc0 (sizeof (GstVideoFrame));

  if (!gst_video_frame_map (scene_info.video_frame, &video_info, buf, (GstMapFlags) (GST_MAP_READ | GST_MAP_GL))) {
    g_warning ("Failed to map the video buffer");
    return TRUE;
  }
  scene_info.texture = *(guint *) scene_info.video_frame->data[0];

  gtk_widget_queue_draw (scene_info.gl_area);
  return TRUE;
}

static gboolean
ensure_gst_glcontext()
{
  scene_info.gst_gl_display = GST_GL_DISPLAY (gst_gl_display_x11_new_with_display (scene_info.display));
  GstGLPlatform gst_gl_platform = GST_GL_PLATFORM_GLX;
  GstGLAPI gst_gl_API = GST_GL_API_OPENGL;

  if (!scene_info.gl_context)
    return FALSE;

  scene_info.gst_gl_context = gst_gl_context_new_wrapped(scene_info.gst_gl_display, (guintptr) scene_info.gl_context, gst_gl_platform, gst_gl_API);
  return TRUE;
}

static GstBusSyncReply
handle_sync_message (GstBus * bus, GstMessage * message, gpointer userData)
{
  const gchar* context_type;

  if (GST_MESSAGE_TYPE (message) != GST_MESSAGE_NEED_CONTEXT)
    return GST_BUS_DROP;

  gst_message_parse_context_type(message, &context_type);

  if (!ensure_gst_glcontext())
    return GST_BUS_DROP;

  if (!g_strcmp0(context_type, GST_GL_DISPLAY_CONTEXT_TYPE)) {
    GstContext* display_context = gst_context_new(GST_GL_DISPLAY_CONTEXT_TYPE, TRUE);
    gst_context_set_gl_display(display_context, scene_info.gst_gl_display);
    gst_element_set_context(GST_ELEMENT(message->src), display_context);
    return GST_BUS_DROP;
  }

  if (!g_strcmp0(context_type, "gst.gl.app_context")) {
      GstContext* app_context = gst_context_new("gst.gl.app_context", TRUE);
      GstStructure* structure = gst_context_writable_structure(app_context);
      gst_structure_set(structure, "context", GST_GL_TYPE_CONTEXT, scene_info.gst_gl_context, NULL);
      gst_element_set_context(GST_ELEMENT(message->src), app_context);
  }

  return GST_BUS_DROP;
}

static void
activate (GtkApplication *app,
          gpointer        user_data)
{
  GtkWidget *window;
  GtkWidget *gl_area;
  GtkWidget *button_box;
  GstElement *pipeline, *videosrc, *glimagesink;
  GstStateChangeReturn ret;
  GstCaps *caps;
  GstBus *bus;

  window = gtk_application_window_new (app);
  gtk_window_set_title (GTK_WINDOW (window), "Window");
  gtk_window_set_default_size (GTK_WINDOW (window), 800, 600);

  gl_area = gtk_gl_area_new ();
  g_signal_connect (gl_area, "render", G_CALLBACK (render), NULL);
  g_signal_connect (gl_area, "realize", G_CALLBACK (realize), NULL);
  g_signal_connect (gl_area, "unrealize", G_CALLBACK (unrealize), NULL);
  gtk_container_add (GTK_CONTAINER (window), gl_area);
  scene_info.gl_area = gl_area;

  gtk_widget_show_all (window);

  scene_info.video_frame = NULL;
  /* create elements */
  pipeline = gst_pipeline_new ("pipeline");
  videosrc = gst_element_factory_make ("videotestsrc", "videotestsrc");
  glimagesink = gst_element_factory_make ("glimagesink", "glimagesink");

  caps = gst_caps_new_simple("video/x-raw",
                             "width", G_TYPE_INT, 640,
                             "height", G_TYPE_INT, 480,
                             "framerate", GST_TYPE_FRACTION, 25, 1,
                             "format", G_TYPE_STRING, "RGBA",
                             NULL) ;

  g_signal_connect_swapped(G_OBJECT(glimagesink), "client-draw", G_CALLBACK (drawCallback), NULL);

  gst_bin_add_many (GST_BIN (pipeline), videosrc, glimagesink, NULL);

  gboolean link_ok = gst_element_link_filtered(videosrc, glimagesink, caps) ;
  gst_caps_unref(caps) ;
  if(!link_ok)
  {
      g_warning("Failed to link videosrc to glimagesink!\n") ;
      return;
  }

  bus = gst_pipeline_get_bus (GST_PIPELINE (pipeline));
  gst_bus_set_sync_handler(bus, (GstBusSyncHandler) (handle_sync_message), NULL, NULL);
  gst_object_unref (bus);

  //start
  ret = gst_element_set_state (pipeline, GST_STATE_PLAYING);
  if (ret == GST_STATE_CHANGE_FAILURE) {
    g_print ("Failed to start up pipeline!\n");
    return;
  }
}

int
main (int    argc,
      char **argv)
{
  GtkApplication *app;
  int status;

  XInitThreads();
  gtk_init (&argc, &argv);
  gst_init (&argc, &argv);

  app = gtk_application_new ("org.gtk.example.glarea", G_APPLICATION_FLAGS_NONE);
  g_signal_connect (app, "activate", G_CALLBACK (activate), NULL);
  status = g_application_run (G_APPLICATION (app), argc, argv);
  g_object_unref (app);

  return status;
}

