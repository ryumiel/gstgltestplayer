#include <gtk/gtk.h>
#include <epoxy/gl.h>

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
  guint vao;
  guint program;
  guint vertex_pos_attrib;
  guint texture_coord_attrib;
} scene_info;

static void
init_buffers (guint  vertex_pos_attrib,
              guint  texture_coord_attrib,
              guint *vao_out)
{
  guint vao, buffer, indice_buffer;

  /* we need to create a VAO to store the other buffers */
  glGenVertexArrays (1, &vao);
  glBindVertexArray (vao);

  /* this is the VBO that holds the vertex data */
  glGenBuffers (1, &buffer);
  glBindBuffer (GL_ARRAY_BUFFER, buffer);
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

  /* reset the state; we will re-enable the VAO when needed */
  glBindBuffer (GL_ARRAY_BUFFER, 0);
  glBindVertexArray (0);

  /* the VBO is referenced by the VAO */
  glDeleteBuffers (1, &buffer);

  if (vao_out != NULL)
    *vao_out = vao;
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
              GError **error)
{
  guint program = 0;
  guint vertex_pos_attrib = 0;
  guint texture_coord_attrib = 0;
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
  init_buffers (scene_info.vertex_pos_attrib,
                scene_info.texture_coord_attrib, &scene_info.vao);
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
  if (scene_info.vao != 0)
    glDeleteVertexArrays (1, &scene_info.vao);
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

  if (scene_info.program == 0 || scene_info.vao == 0)
    return TRUE;

  /* load our program */
  glUseProgram (scene_info.program);

  /* use the buffers in the VAO */
  glBindVertexArray (scene_info.vao);

  /* draw the three vertices as a triangle */
  glDrawElements (GL_TRIANGLES, 6, GL_UNSIGNED_SHORT, 0);

  /* we finished using the buffers and program */
  glBindVertexArray (0);
  glUseProgram (0);

  // we completed our drawing; the draw commands will be
  // flushed at the end of the signal emission chain, and
  // the buffers will be drawn on the window
  return TRUE;
}

static void
activate (GtkApplication *app,
          gpointer        user_data)
{
  GtkWidget *window;
  GtkWidget *gl_area;
  GtkWidget *button_box;

  window = gtk_application_window_new (app);
  gtk_window_set_title (GTK_WINDOW (window), "Window");
  gtk_window_set_default_size (GTK_WINDOW (window), 800, 600);

  gl_area = gtk_gl_area_new ();
  g_signal_connect (gl_area, "render", G_CALLBACK (render), NULL);
  g_signal_connect (gl_area, "realize", G_CALLBACK (realize), NULL);
  g_signal_connect (gl_area, "unrealize", G_CALLBACK (unrealize), NULL);
  gtk_container_add (GTK_CONTAINER (window), gl_area);

  gtk_widget_show_all (window);
}

int
main (int    argc,
      char **argv)
{
  GtkApplication *app;
  int status;

  app = gtk_application_new ("org.gtk.example.glarea", G_APPLICATION_FLAGS_NONE);
  g_signal_connect (app, "activate", G_CALLBACK (activate), NULL);
  status = g_application_run (G_APPLICATION (app), argc, argv);
  g_object_unref (app);

  return status;
}

