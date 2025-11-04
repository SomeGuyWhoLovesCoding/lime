package lime.system;

import lime.graphics.PixelFormat;

#if !lime_debug
@:fileXml('tags="haxe,release"')
@:noDebug
#end
class DisplayMode
{
	/**
	 * vertical resolution
	**/
	public var height(default, null):Int;

	/**
	 * pixel format
	**/
	public var pixelFormat(default, null):PixelFormat;

	/**
	 * refresh rate in Hz
	 * (changed to a Float to keep consistency in check)
	**/
	public var refreshRate(default, null):Float;

	/**
	 * horizontal resolution
	**/
	public var width(default, null):Int;

	@:noCompletion private function new(width:Int, height:Int, refreshRate:Float, pixelFormat:PixelFormat)
	{
		this.width = width;
		this.height = height;
		this.refreshRate = refreshRate;
		this.pixelFormat = pixelFormat;
	}
}
