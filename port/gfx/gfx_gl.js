// port/gfx (S3b): the WebGL2 context for the renderer (link with --js-library port/gfx/gfx_gl.js).
// retrowser (video "webgl") hands the module an OffscreenCanvas as Module.canvas inside its worker: there is no DOM,
// so "#canvas" lookups find nothing and the context is made on that object directly. A page build or the test
// harness (port/gfx/test) sets Module.canvas the same way.
addToLibrary({
  port_gfx_gl_create__deps: ['$GL'],
  port_gfx_gl_create: () => {
    const canvas = Module['canvas'];
    if (!canvas || typeof canvas.getContext !== 'function') return 0;
    return GL.createContext(canvas, {
      majorVersion: 2, minorVersion: 0, alpha: false, depth: false, stencil: false, antialias: false,
      premultipliedAlpha: true, preserveDrawingBuffer: true, powerPreference: 'default',
      failIfMajorPerformanceCaveat: false, enableExtensionsByDefault: true,
    });
  },
});
