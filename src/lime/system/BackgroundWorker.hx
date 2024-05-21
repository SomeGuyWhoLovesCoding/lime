package lime.system;

#if !lime_debug
@:fileXml('tags="haxe,release"')
@:noDebug
#end
typedef BackgroundWorker = lime.system.ThreadPool;