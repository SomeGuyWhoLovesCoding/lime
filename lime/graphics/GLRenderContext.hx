package lime.graphics;


#if (sys && !display)
typedef GLRenderContext = lime._backend.native.NativeGLRenderContext;
#elseif ((js && html5) && !display)
typedef GLRenderContext = lime._backend.html5.HTML5GLRenderContext;
#else


import lime.graphics.opengl.GLActiveInfo;
import lime.graphics.opengl.GLBuffer;
import lime.graphics.opengl.GLContextAttributes;
import lime.graphics.opengl.GLContextType;
import lime.graphics.opengl.GLFramebuffer;
import lime.graphics.opengl.GLProgram;
import lime.graphics.opengl.GLRenderbuffer;
import lime.graphics.opengl.GLShader;
import lime.graphics.opengl.GLShaderPrecisionFormat;
import lime.graphics.opengl.GLTexture;
import lime.graphics.opengl.GLUniformLocation;

#if (js && html5)
import js.html.CanvasElement;
#end

#if (js && html5)
@:native("WebGLRenderingContext")
#end

extern class GLRenderContext {
	
	
	public var ACTIVE_ATTRIBUTES:Int;
	public var ACTIVE_TEXTURE:Int;
	public var ACTIVE_UNIFORMS:Int;
	public var ALIASED_LINE_WIDTH_RANGE:Int;
	public var ALIASED_POINT_SIZE_RANGE:Int;
	public var ALPHA:Int;
	public var ALPHA_BITS:Int;
	public var ALWAYS:Int;
	public var ARRAY_BUFFER:Int;
	public var ARRAY_BUFFER_BINDING:Int;
	public var ATTACHED_SHADERS:Int;
	public var BACK:Int;
	public var BLEND:Int;
	public var BLEND_COLOR:Int;
	public var BLEND_DST_ALPHA:Int;
	public var BLEND_DST_RGB:Int;
	public var BLEND_EQUATION:Int;
	public var BLEND_EQUATION_ALPHA:Int;
	public var BLEND_EQUATION_RGB:Int;
	public var BLEND_SRC_ALPHA:Int;
	public var BLEND_SRC_RGB:Int;
	public var BLUE_BITS:Int;
	public var BOOL:Int;
	public var BOOL_VEC2:Int;
	public var BOOL_VEC3:Int;
	public var BOOL_VEC4:Int;
	public var BROWSER_DEFAULT_WEBGL:Int;
	public var BUFFER_SIZE:Int;
	public var BUFFER_USAGE:Int;
	public var BYTE:Int;
	public var CCW:Int;
	public var CLAMP_TO_EDGE:Int;
	public var COLOR_ATTACHMENT0:Int;
	public var COLOR_BUFFER_BIT:Int;
	public var COLOR_CLEAR_VALUE:Int;
	public var COLOR_WRITEMASK:Int;
	public var COMPILE_STATUS:Int;
	public var COMPRESSED_TEXTURE_FORMATS:Int;
	public var CONSTANT_ALPHA:Int;
	public var CONSTANT_COLOR:Int;
	public var CONTEXT_LOST_WEBGL:Int;
	public var CULL_FACE:Int;
	public var CULL_FACE_MODE:Int;
	public var CURRENT_PROGRAM:Int;
	public var CURRENT_VERTEX_ATTRIB:Int;
	public var CW:Int;
	public var DECR:Int;
	public var DECR_WRAP:Int;
	public var DELETE_STATUS:Int;
	public var DEPTH_ATTACHMENT:Int;
	public var DEPTH_BITS:Int;
	public var DEPTH_BUFFER_BIT:Int;
	public var DEPTH_CLEAR_VALUE:Int;
	public var DEPTH_COMPONENT:Int;
	public var DEPTH_COMPONENT16:Int;
	public var DEPTH_FUNC:Int;
	public var DEPTH_RANGE:Int;
	public var DEPTH_STENCIL:Int;
	public var DEPTH_STENCIL_ATTACHMENT:Int;
	public var DEPTH_TEST:Int;
	public var DEPTH_WRITEMASK:Int;
	public var DITHER:Int;
	public var DONT_CARE:Int;
	public var DST_ALPHA:Int;
	public var DST_COLOR:Int;
	public var DYNAMIC_DRAW:Int;
	public var ELEMENT_ARRAY_BUFFER:Int;
	public var ELEMENT_ARRAY_BUFFER_BINDING:Int;
	public var EQUAL:Int;
	public var FASTEST:Int;
	public var FLOAT:Int;
	public var FLOAT_MAT2:Int;
	public var FLOAT_MAT3:Int;
	public var FLOAT_MAT4:Int;
	public var FLOAT_VEC2:Int;
	public var FLOAT_VEC3:Int;
	public var FLOAT_VEC4:Int;
	public var FRAGMENT_SHADER:Int;
	public var FRAMEBUFFER:Int;
	public var FRAMEBUFFER_ATTACHMENT_OBJECT_NAME:Int;
	public var FRAMEBUFFER_ATTACHMENT_OBJECT_TYPE:Int;
	public var FRAMEBUFFER_ATTACHMENT_TEXTURE_CUBE_MAP_FACE:Int;
	public var FRAMEBUFFER_ATTACHMENT_TEXTURE_LEVEL:Int;
	public var FRAMEBUFFER_BINDING:Int;
	public var FRAMEBUFFER_COMPLETE:Int;
	public var FRAMEBUFFER_INCOMPLETE_ATTACHMENT:Int;
	public var FRAMEBUFFER_INCOMPLETE_DIMENSIONS:Int;
	public var FRAMEBUFFER_INCOMPLETE_MISSING_ATTACHMENT:Int;
	public var FRAMEBUFFER_UNSUPPORTED:Int;
	public var FRONT:Int;
	public var FRONT_AND_BACK:Int;
	public var FRONT_FACE:Int;
	public var FUNC_ADD:Int;
	public var FUNC_REVERSE_SUBTRACT:Int;
	public var FUNC_SUBTRACT:Int;
	public var GENERATE_MIPMAP_HINT:Int;
	public var GEQUAL:Int;
	public var GREATER:Int;
	public var GREEN_BITS:Int;
	public var HIGH_FLOAT:Int;
	public var HIGH_INT:Int;
	public var INCR:Int;
	public var INCR_WRAP:Int;
	public var INT:Int;
	public var INT_VEC2:Int;
	public var INT_VEC3:Int;
	public var INT_VEC4:Int;
	public var INVALID_ENUM:Int;
	public var INVALID_FRAMEBUFFER_OPERATION:Int;
	public var INVALID_OPERATION:Int;
	public var INVALID_VALUE:Int;
	public var INVERT:Int;
	public var KEEP:Int;
	public var LEQUAL:Int;
	public var LESS:Int;
	public var LINEAR:Int;
	public var LINEAR_MIPMAP_LINEAR:Int;
	public var LINEAR_MIPMAP_NEAREST:Int;
	public var LINES:Int;
	public var LINE_LOOP:Int;
	public var LINE_STRIP:Int;
	public var LINE_WIDTH:Int;
	public var LINK_STATUS:Int;
	public var LOW_FLOAT:Int;
	public var LOW_INT:Int;
	public var LUMINANCE:Int;
	public var LUMINANCE_ALPHA:Int;
	public var MAX_COMBINED_TEXTURE_IMAGE_UNITS:Int;
	public var MAX_CUBE_MAP_TEXTURE_SIZE:Int;
	public var MAX_FRAGMENT_UNIFORM_VECTORS:Int;
	public var MAX_RENDERBUFFER_SIZE:Int;
	public var MAX_TEXTURE_IMAGE_UNITS:Int;
	public var MAX_TEXTURE_SIZE:Int;
	public var MAX_VARYING_VECTORS:Int;
	public var MAX_VERTEX_ATTRIBS:Int;
	public var MAX_VERTEX_TEXTURE_IMAGE_UNITS:Int;
	public var MAX_VERTEX_UNIFORM_VECTORS:Int;
	public var MAX_VIEWPORT_DIMS:Int;
	public var MEDIUM_FLOAT:Int;
	public var MEDIUM_INT:Int;
	public var MIRRORED_REPEAT:Int;
	public var NEAREST:Int;
	public var NEAREST_MIPMAP_LINEAR:Int;
	public var NEAREST_MIPMAP_NEAREST:Int;
	public var NEVER:Int;
	public var NICEST:Int;
	public var NONE:Int;
	public var NOTEQUAL:Int;
	public var NO_ERROR:Int;
	public var ONE:Int;
	public var ONE_MINUS_CONSTANT_ALPHA:Int;
	public var ONE_MINUS_CONSTANT_COLOR:Int;
	public var ONE_MINUS_DST_ALPHA:Int;
	public var ONE_MINUS_DST_COLOR:Int;
	public var ONE_MINUS_SRC_ALPHA:Int;
	public var ONE_MINUS_SRC_COLOR:Int;
	public var OUT_OF_MEMORY:Int;
	public var PACK_ALIGNMENT:Int;
	public var POINTS:Int;
	public var POLYGON_OFFSET_FACTOR:Int;
	public var POLYGON_OFFSET_FILL:Int;
	public var POLYGON_OFFSET_UNITS:Int;
	public var RED_BITS:Int;
	public var RENDERBUFFER:Int;
	public var RENDERBUFFER_ALPHA_SIZE:Int;
	public var RENDERBUFFER_BINDING:Int;
	public var RENDERBUFFER_BLUE_SIZE:Int;
	public var RENDERBUFFER_DEPTH_SIZE:Int;
	public var RENDERBUFFER_GREEN_SIZE:Int;
	public var RENDERBUFFER_HEIGHT:Int;
	public var RENDERBUFFER_INTERNAL_FORMAT:Int;
	public var RENDERBUFFER_RED_SIZE:Int;
	public var RENDERBUFFER_STENCIL_SIZE:Int;
	public var RENDERBUFFER_WIDTH:Int;
	public var RENDERER:Int;
	public var REPEAT:Int;
	public var REPLACE:Int;
	public var RGB:Int;
	public var RGB565:Int;
	public var RGB5_A1:Int;
	public var RGBA:Int;
	public var RGBA4:Int;
	public var SAMPLER_2D:Int;
	public var SAMPLER_CUBE:Int;
	public var SAMPLES:Int;
	public var SAMPLE_ALPHA_TO_COVERAGE:Int;
	public var SAMPLE_BUFFERS:Int;
	public var SAMPLE_COVERAGE:Int;
	public var SAMPLE_COVERAGE_INVERT:Int;
	public var SAMPLE_COVERAGE_VALUE:Int;
	public var SCISSOR_BOX:Int;
	public var SCISSOR_TEST:Int;
	public var SHADER_TYPE:Int;
	public var SHADING_LANGUAGE_VERSION:Int;
	public var SHORT:Int;
	public var SRC_ALPHA:Int;
	public var SRC_ALPHA_SATURATE:Int;
	public var SRC_COLOR:Int;
	public var STATIC_DRAW:Int;
	public var STENCIL_ATTACHMENT:Int;
	public var STENCIL_BACK_FAIL:Int;
	public var STENCIL_BACK_FUNC:Int;
	public var STENCIL_BACK_PASS_DEPTH_FAIL:Int;
	public var STENCIL_BACK_PASS_DEPTH_PASS:Int;
	public var STENCIL_BACK_REF:Int;
	public var STENCIL_BACK_VALUE_MASK:Int;
	public var STENCIL_BACK_WRITEMASK:Int;
	public var STENCIL_BITS:Int;
	public var STENCIL_BUFFER_BIT:Int;
	public var STENCIL_CLEAR_VALUE:Int;
	public var STENCIL_FAIL:Int;
	public var STENCIL_FUNC:Int;
	public var STENCIL_INDEX:Int;
	public var STENCIL_INDEX8:Int;
	public var STENCIL_PASS_DEPTH_FAIL:Int;
	public var STENCIL_PASS_DEPTH_PASS:Int;
	public var STENCIL_REF:Int;
	public var STENCIL_TEST:Int;
	public var STENCIL_VALUE_MASK:Int;
	public var STENCIL_WRITEMASK:Int;
	public var STREAM_DRAW:Int;
	public var SUBPIXEL_BITS:Int;
	public var TEXTURE:Int;
	public var TEXTURE0:Int;
	public var TEXTURE1:Int;
	public var TEXTURE10:Int;
	public var TEXTURE11:Int;
	public var TEXTURE12:Int;
	public var TEXTURE13:Int;
	public var TEXTURE14:Int;
	public var TEXTURE15:Int;
	public var TEXTURE16:Int;
	public var TEXTURE17:Int;
	public var TEXTURE18:Int;
	public var TEXTURE19:Int;
	public var TEXTURE2:Int;
	public var TEXTURE20:Int;
	public var TEXTURE21:Int;
	public var TEXTURE22:Int;
	public var TEXTURE23:Int;
	public var TEXTURE24:Int;
	public var TEXTURE25:Int;
	public var TEXTURE26:Int;
	public var TEXTURE27:Int;
	public var TEXTURE28:Int;
	public var TEXTURE29:Int;
	public var TEXTURE3:Int;
	public var TEXTURE30:Int;
	public var TEXTURE31:Int;
	public var TEXTURE4:Int;
	public var TEXTURE5:Int;
	public var TEXTURE6:Int;
	public var TEXTURE7:Int;
	public var TEXTURE8:Int;
	public var TEXTURE9:Int;
	public var TEXTURE_2D:Int;
	public var TEXTURE_BINDING_2D:Int;
	public var TEXTURE_BINDING_CUBE_MAP:Int;
	public var TEXTURE_CUBE_MAP:Int;
	public var TEXTURE_CUBE_MAP_NEGATIVE_X:Int;
	public var TEXTURE_CUBE_MAP_NEGATIVE_Y:Int;
	public var TEXTURE_CUBE_MAP_NEGATIVE_Z:Int;
	public var TEXTURE_CUBE_MAP_POSITIVE_X:Int;
	public var TEXTURE_CUBE_MAP_POSITIVE_Y:Int;
	public var TEXTURE_CUBE_MAP_POSITIVE_Z:Int;
	public var TEXTURE_MAG_FILTER:Int;
	public var TEXTURE_MIN_FILTER:Int;
	public var TEXTURE_WRAP_S:Int;
	public var TEXTURE_WRAP_T:Int;
	public var TRIANGLES:Int;
	public var TRIANGLE_FAN:Int;
	public var TRIANGLE_STRIP:Int;
	public var UNPACK_ALIGNMENT:Int;
	public var UNPACK_COLORSPACE_CONVERSION_WEBGL:Int;
	public var UNPACK_FLIP_Y_WEBGL:Int;
	public var UNPACK_PREMULTIPLY_ALPHA_WEBGL:Int;
	public var UNSIGNED_BYTE:Int;
	public var UNSIGNED_INT:Int;
	public var UNSIGNED_SHORT:Int;
	public var UNSIGNED_SHORT_4_4_4_4:Int;
	public var UNSIGNED_SHORT_5_5_5_1:Int;
	public var UNSIGNED_SHORT_5_6_5:Int;
	public var VALIDATE_STATUS:Int;
	public var VENDOR:Int;
	public var VERSION:Int;
	public var VERTEX_ATTRIB_ARRAY_BUFFER_BINDING:Int;
	public var VERTEX_ATTRIB_ARRAY_ENABLED:Int;
	public var VERTEX_ATTRIB_ARRAY_NORMALIZED:Int;
	public var VERTEX_ATTRIB_ARRAY_POINTER:Int;
	public var VERTEX_ATTRIB_ARRAY_SIZE:Int;
	public var VERTEX_ATTRIB_ARRAY_STRIDE:Int;
	public var VERTEX_ATTRIB_ARRAY_TYPE:Int;
	public var VERTEX_SHADER:Int;
	public var VIEWPORT:Int;
	public var ZERO:Int;
	
	#if (js && html5)
	public var canvas (get, never):CanvasElement;
	public var drawingBufferHeight (get, never):Int;
	public var drawingBufferWidth (get, never):Int;
	#end
	
	public var type (default, null):GLContextType;
	public var version (default, null):Float;
	
	private function new ();
	
	public function activeTexture (texture:Int):Void;
	public function attachShader (program:GLProgram, shader:GLShader):Void;
	public function bindAttribLocation (program:GLProgram, index:Int, name:String):Void;
	public function bindBuffer (target:Int, buffer:GLBuffer):Void;
	public function bindFramebuffer (target:Int, framebuffer:GLFramebuffer):Void;
	public function bindRenderbuffer (target:Int, renderbuffer:GLRenderbuffer):Void;
	public function bindTexture (target:Int, texture:GLTexture):Void;
	public function blendColor (red:Float, green:Float, blue:Float, alpha:Float):Void;
	public function blendEquation (mode:Int):Void;
	public function blendEquationSeparate (modeRGB:Int, modeAlpha:Int):Void;
	public function blendFunc (sfactor:Int, dfactor:Int):Void;
	public function blendFuncSeparate (srcRGB:Int, dstRGB:Int, srcAlpha:Int, dstAlpha:Int):Void;
	public function bufferData (target:Int, size:Int, srcData:lime.utils.ArrayBufferView, usage:Int, srcOffset:Int = 0, length:Int = 0):Void;
	public function bufferSubData (target:Int, dstByteOffset:Int, size:Int, srcData:lime.utils.ArrayBufferView, srcOffset:Int = 0, length:Int = 0):Void;
	public function checkFramebufferStatus (target:Int):Int;
	public function clear (mask:Int):Void;
	public function clearColor (red:Float, green:Float, blue:Float, alpha:Float):Void;
	public function clearDepth (depth:Float):Void;
	public function clearStencil (s:Int):Void;
	public function colorMask (red:Bool, green:Bool, blue:Bool, alpha:Bool):Void;
	public function compileShader (shader:GLShader):Void;
	public function compressedTexImage2D (target:Int, level:Int, internalformat:Int, width:Int, height:Int, border:Int, srcData:lime.utils.ArrayBufferView, srcOffset:Int = 0, length:Int = 0):Void;
	public function compressedTexSubImage2D (target:Int, level:Int, xoffset:Int, yoffset:Int, width:Int, height:Int, format:Int, srcData:lime.utils.ArrayBufferView, srcOffset:Int = 0, length:Int = 0):Void;
	public function copyTexImage2D (target:Int, level:Int, internalformat:Int, x:Int, y:Int, width:Int, height:Int, border:Int):Void;
	public function copyTexSubImage2D (target:Int, level:Int, xoffset:Int, yoffset:Int, x:Int, y:Int, width:Int, height:Int):Void;
	public function createBuffer ():GLBuffer;
	public function createFramebuffer ():GLFramebuffer;
	public function createProgram ():GLProgram;
	public function createRenderbuffer ():GLRenderbuffer;
	public function createShader (type:Int):GLShader;
	public function createTexture ():GLTexture;
	public function cullFace (mode:Int):Void;
	public function deleteBuffer (buffer:GLBuffer):Void;
	public function deleteFramebuffer (framebuffer:GLFramebuffer):Void;
	public function deleteProgram (program:GLProgram):Void;
	public function deleteRenderbuffer (renderbuffer:GLRenderbuffer):Void;
	public function deleteShader (shader:GLShader):Void;
	public function deleteTexture (texture:GLTexture):Void;
	public function depthFunc (func:Int):Void;
	public function depthMask (flag:Bool):Void;
	public function depthRange (zNear:Float, zFar:Float):Void;
	public function detachShader (program:GLProgram, shader:GLShader):Void;
	public function disable (cap:Int):Void;
	public function disableVertexAttribArray (index:Int):Void;
	public function drawArrays (mode:Int, first:Int, count:Int):Void;
	public function drawElements (mode:Int, count:Int, type:Int, offset:Int):Void;
	public function enable (cap:Int):Void;
	public function enableVertexAttribArray (index:Int):Void;
	public function finish ():Void;
	public function flush ():Void;
	public function framebufferRenderbuffer (target:Int, attachment:Int, renderbuffertarget:Int, renderbuffer:GLRenderbuffer):Void;
	public function framebufferTexture2D (target:Int, attachment:Int, textarget:Int, texture:GLTexture, level:Int):Void;
	public function frontFace (mode:Int):Void;
	public function generateMipmap (target:Int):Void;
	public function getActiveAttrib (program:GLProgram, index:Int):GLActiveInfo;
	public function getActiveUniform (program:GLProgram, index:Int):GLActiveInfo;
	public function getAttachedShaders (program:GLProgram):Array<GLShader>;
	public function getAttribLocation (program:GLProgram, name:String):Int;
	public function getBufferParameter (target:Int, pname:Int):Int;
	public function getContextAttributes ():GLContextAttributes;
	public function getError ():Int;
	public function getExtension (name:String):Dynamic;
	public function getFramebufferAttachmentParameter (target:Int, attachment:Int, pname:Int):Dynamic;
	public function getParameter (pname:Int):Dynamic;
	public function getProgramInfoLog (program:GLProgram):String;
	public function getProgramParameter (program:GLProgram, pname:Int):Dynamic;
	public function getRenderbufferParameter (target:Int, pname:Int):Int;
	public function getShaderInfoLog (shader:GLShader):String;
	public function getShaderParameter (shader:GLShader, pname:Int):Dynamic;
	public function getShaderPrecisionFormat (shadertype:Int, precisiontype:Int):GLShaderPrecisionFormat;
	public function getShaderSource (shader:GLShader):String;
	public function getSupportedExtensions ():Array<String>;
	public function getTexParameter (target:Int, pname:Int):Dynamic;
	public function getUniform (program:GLProgram, location:GLUniformLocation):Dynamic;
	public function getUniformLocation (program:GLProgram, name:String):GLUniformLocation;
	public function getVertexAttrib (index:Int, pname:Int):Dynamic;
	public function getVertexAttribOffset (index:Int, pname:Int):Int;
	public function hint (target:Int, mode:Int):Void;
	public function isBuffer (buffer:GLBuffer):Bool;
	public function isContextLost ():Bool;
	public function isEnabled (cap:Int):Bool;
	public function isFramebuffer (framebuffer:GLFramebuffer):Bool;
	public function isProgram (program:GLProgram):Bool;
	public function isRenderbuffer (renderbuffer:GLRenderbuffer):Bool;
	public function isShader (shader:GLShader):Bool;
	public function isTexture (texture:GLTexture):Bool;
	public function lineWidth (width:Float):Void;
	public function linkProgram (program:GLProgram):Void;
	public function pixelStorei (pname:Int, param:Int):Void;
	public function polygonOffset (factor:Float, units:Float):Void;
	public function readPixels (x:Int, y:Int, width:Int, height:Int, format:Int, type:Int, pixels:lime.utils.ArrayBufferView):Void;
	public function releaseShaderCompiler ():Void;
	public function renderbufferStorage (target:Int, internalformat:Int, width:Int, height:Int):Void;
	public function sampleCoverage (value:Float, invert:Bool):Void;
	public function scissor (x:Int, y:Int, width:Int, height:Int):Void;
	public function shaderSource (shader:GLShader, string:String):Void;
	public function stencilFunc (func:Int, ref:Int, mask:Int):Void;
	public function stencilFuncSeparate (face:Int, func:Int, ref:Int, mask:Int):Void;
	public function stencilMask (mask:Int):Void;
	public function stencilMaskSeparate (face:Int, mask:Int):Void;
	public function stencilOp (fail:Int, zfail:Int, zpass:Int):Void;
	public function stencilOpSeparate (face:Int, fail:Int, zfail:Int, zpass:Int):Void;
	public function texImage2D (target:Int, level:Int, internalformat:Int, width:Int, height:Int, border:Int, format:Int, type:Int, srcData:lime.utils.ArrayBufferView, srcOffset:Int = 0):Void;
	public function texParameterf (target:Int, pname:Int, param:Float):Void;
	public function texParameteri (target:Int, pname:Int, param:Int):Void;
	public function texSubImage2D (target:Int, level:Int, xoffset:Int, yoffset:Int, width:Int, height:Int, format:Int, type:Int, srcData:lime.utils.ArrayBufferView, srcOffset:Int = 0):Void;
	public function uniform1f (location:GLUniformLocation, x:Float):Void;
	public function uniform1fv (location:GLUniformLocation, v:lime.utils.Float32Array):Void;
	public function uniform1i (location:GLUniformLocation, x:Int):Void;
	public function uniform1iv (location:GLUniformLocation, v:lime.utils.Int32Array):Void;
	public function uniform2f (location:GLUniformLocation, x:Float, y:Float):Void;
	public function uniform2fv (location:GLUniformLocation, v:lime.utils.Float32Array):Void;
	public function uniform2i (location:GLUniformLocation, x:Int, y:Int):Void;
	public function uniform2iv (location:GLUniformLocation, v:lime.utils.Int32Array):Void;
	public function uniform3f (location:GLUniformLocation, x:Float, y:Float, z:Float):Void;
	public function uniform3fv(location:GLUniformLocation, v:lime.utils.Float32Array):Void;
	public function uniform3i (location:GLUniformLocation, x:Int, y:Int, z:Int):Void;
	public function uniform3iv (location:GLUniformLocation, v:lime.utils.Int32Array):Void;
	public function uniform4f (location:GLUniformLocation, x:Float, y:Float, z:Float, w:Float):Void;
	public function uniform4fv (location:GLUniformLocation, v:lime.utils.Float32Array):Void;
	public function uniform4i (location:GLUniformLocation, x:Int, y:Int, z:Int, w:Int):Void;
	public function uniform4iv (location:GLUniformLocation, v:lime.utils.Int32Array):Void;
	public function uniformMatrix2fv (location:GLUniformLocation, transpose:Bool, array:lime.utils.Float32Array):Void;
	public function uniformMatrix3fv (location:GLUniformLocation, transpose:Bool, array:lime.utils.Float32Array):Void;
	public function uniformMatrix4fv (location:GLUniformLocation, transpose:Bool, array:lime.utils.Float32Array):Void;
	public function useProgram (program:GLProgram):Void;
	public function validateProgram (program:GLProgram):Void;
	public function vertexAttrib1f (indx:Int, x:Float):Void;
	public function vertexAttrib1fv (indx:Int, values:lime.utils.Float32Array):Void;
	public function vertexAttrib2f (indx:Int, x:Float, y:Float):Void;
	public function vertexAttrib2fv (indx:Int, values:lime.utils.Float32Array):Void;
	public function vertexAttrib3f (indx:Int, x:Float, y:Float, z:Float):Void;
	public function vertexAttrib3fv (indx:Int, values:lime.utils.Float32Array):Void;
	public function vertexAttrib4f (indx:Int, x:Float, y:Float, z:Float, w:Float):Void;
	public function vertexAttrib4fv (indx:Int, values:lime.utils.Float32Array):Void;
	public function vertexAttribPointer (indx:Int, size:Int, type:Int, normalized:Bool, stride:Int, offset:Int):Void;
	public function viewport (x:Int, y:Int, width:Int, height:Int):Void;
	
}


#end