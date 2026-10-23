/* Web (Emscripten) build: a no-op GLFW. See CMakeLists.txt in this directory.
 *
 * Only the functions the engine, the renderer and the tests call are defined. glfwInit fails, so the windowed path
 * (which needs a window the browser build does not have yet) reports an initialisation failure, and the headless path
 * (which never calls GLFW) is the one that runs. Every other function is safe to call with the null window or monitor
 * that results, and reports "nothing": no key or button down, no joystick, no monitor.
 */
#include <GLFW/glfw3.h>

#include <stddef.h>

static GLFWerrorfun g_errorCallback = NULL;

int glfwInit(void) { return GLFW_FALSE; }
void glfwTerminate(void) {}

GLFWerrorfun glfwSetErrorCallback(GLFWerrorfun callback)
{
    GLFWerrorfun previous = g_errorCallback;
    g_errorCallback = callback;
    return previous;
}

void glfwWindowHint(int hint, int value) { (void)hint; (void)value; }

GLFWwindow *glfwCreateWindow(int width, int height, const char *title, GLFWmonitor *monitor, GLFWwindow *share)
{
    (void)width; (void)height; (void)title; (void)monitor; (void)share;
    return NULL;
}

void glfwDestroyWindow(GLFWwindow *window) { (void)window; }
int glfwWindowShouldClose(GLFWwindow *window) { (void)window; return GLFW_TRUE; }
void glfwSetWindowTitle(GLFWwindow *window, const char *title) { (void)window; (void)title; }

void glfwGetWindowPos(GLFWwindow *window, int *xpos, int *ypos)
{
    (void)window;
    if (xpos) *xpos = 0;
    if (ypos) *ypos = 0;
}

void glfwGetWindowSize(GLFWwindow *window, int *width, int *height)
{
    (void)window;
    if (width) *width = 0;
    if (height) *height = 0;
}

void glfwGetFramebufferSize(GLFWwindow *window, int *width, int *height)
{
    (void)window;
    if (width) *width = 0;
    if (height) *height = 0;
}

void glfwSetWindowAttrib(GLFWwindow *window, int attrib, int value) { (void)window; (void)attrib; (void)value; }

void glfwSetWindowMonitor(GLFWwindow *window, GLFWmonitor *monitor, int xpos, int ypos, int width, int height,
                          int refreshRate)
{
    (void)window; (void)monitor; (void)xpos; (void)ypos; (void)width; (void)height; (void)refreshRate;
}

GLFWmonitor *glfwGetPrimaryMonitor(void) { return NULL; }

GLFWmonitor **glfwGetMonitors(int *count)
{
    if (count) *count = 0;
    return NULL;
}

void glfwGetMonitorPos(GLFWmonitor *monitor, int *xpos, int *ypos)
{
    (void)monitor;
    if (xpos) *xpos = 0;
    if (ypos) *ypos = 0;
}

const GLFWvidmode *glfwGetVideoMode(GLFWmonitor *monitor) { (void)monitor; return NULL; }

static void *g_userPointer = NULL;
void glfwSetWindowUserPointer(GLFWwindow *window, void *pointer) { (void)window; g_userPointer = pointer; }
void *glfwGetWindowUserPointer(GLFWwindow *window) { (void)window; return g_userPointer; }

GLFWframebuffersizefun glfwSetFramebufferSizeCallback(GLFWwindow *window, GLFWframebuffersizefun callback)
{
    (void)window; (void)callback;
    return NULL;
}

GLFWscrollfun glfwSetScrollCallback(GLFWwindow *window, GLFWscrollfun callback)
{
    (void)window; (void)callback;
    return NULL;
}

void glfwSetInputMode(GLFWwindow *window, int mode, int value) { (void)window; (void)mode; (void)value; }
int glfwGetKey(GLFWwindow *window, int key) { (void)window; (void)key; return GLFW_RELEASE; }
int glfwGetMouseButton(GLFWwindow *window, int button) { (void)window; (void)button; return GLFW_RELEASE; }

void glfwGetCursorPos(GLFWwindow *window, double *xpos, double *ypos)
{
    (void)window;
    if (xpos) *xpos = 0.0;
    if (ypos) *ypos = 0.0;
}

int glfwJoystickPresent(int jid) { (void)jid; return GLFW_FALSE; }
int glfwJoystickIsGamepad(int jid) { (void)jid; return GLFW_FALSE; }
const char *glfwGetGamepadName(int jid) { (void)jid; return NULL; }
const char *glfwGetJoystickName(int jid) { (void)jid; return NULL; }
int glfwGetGamepadState(int jid, GLFWgamepadstate *state) { (void)jid; (void)state; return GLFW_FALSE; }

void glfwPollEvents(void) {}
void glfwMakeContextCurrent(GLFWwindow *window) { (void)window; }
void glfwSwapBuffers(GLFWwindow *window) { (void)window; }
GLFWglproc glfwGetProcAddress(const char *procname) { (void)procname; return NULL; }
