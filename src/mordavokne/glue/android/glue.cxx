#include <cerrno>
#include <ctime>
#include <csignal>

#include <android/native_activity.h>
#include <android/configuration.h>
#include <android/asset_manager.h>
#include <android/window.h>

#include <unikod/utf8.hpp>
#include <nitki/Queue.hpp>

#include <sys/eventfd.h>

#include "../../application.hpp"
#include "../../factory.hpp"

#include <mordaren/OpenGLES2Renderer.hpp>

#include <EGL/egl.h>

#include "../friendAccessors.cxx"

using namespace mordavokne;

namespace{
ANativeActivity* nativeActivity = 0;

mordavokne::application& getApp(ANativeActivity* activity){
	ASSERT(activity)
	ASSERT(activity->instance)
	return *static_cast<mordavokne::application*>(activity->instance);
}

ANativeWindow* androidWindow = 0;


class JavaFunctionsWrapper : public utki::Unique{
	JNIEnv *env;
	jclass clazz;
	jobject obj;

	jmethodID resolveKeycodeUnicodeMeth;

	jmethodID getDotsPerInchMeth;

	jmethodID showVirtualKeyboardMeth;
	jmethodID hideVirtualKeyboardMeth;

	jmethodID listDirContentsMeth;

	jmethodID getStorageDirMeth;
public:
	JavaFunctionsWrapper(ANativeActivity* a){
		this->env = a->env;
		this->obj = a->clazz;
		this->clazz = this->env->GetObjectClass(this->obj);
		ASSERT(this->clazz)


		this->resolveKeycodeUnicodeMeth = this->env->GetMethodID(this->clazz, "resolveKeyUnicode", "(III)I");
		ASSERT(this->resolveKeycodeUnicodeMeth)

		this->getDotsPerInchMeth = this->env->GetMethodID(this->clazz, "getDotsPerInch", "()F");

		this->listDirContentsMeth = this->env->GetMethodID(this->clazz, "listDirContents", "(Ljava/lang/String;)[Ljava/lang/String;");
		ASSERT(this->listDirContentsMeth)

		this->showVirtualKeyboardMeth = this->env->GetMethodID(this->clazz, "showVirtualKeyboard", "()V");
		ASSERT(this->showVirtualKeyboardMeth)

		this->hideVirtualKeyboardMeth = this->env->GetMethodID(this->clazz, "hideVirtualKeyboard", "()V");
		ASSERT(this->hideVirtualKeyboardMeth)

		this->getStorageDirMeth = this->env->GetMethodID(this->clazz, "getStorageDir", "()Ljava/lang/String;");
		ASSERT(this->getStorageDirMeth)
	}

	~JavaFunctionsWrapper()noexcept{
	}

	char32_t resolveKeyUnicode(int32_t devId, int32_t metaState, int32_t keyCode){
		return char32_t(this->env->CallIntMethod(
				this->obj,
				this->resolveKeycodeUnicodeMeth,
				jint(devId),
				jint(metaState),
				jint(keyCode)
			));
	}

	float getDotsPerInch(){
		return float(this->env->CallFloatMethod(this->obj, this->getDotsPerInchMeth));
	}

	void hide_virtual_keyboard(){
		this->env->CallVoidMethod(this->obj, this->hideVirtualKeyboardMeth);
	}

	void show_virtual_keyboard(){
		this->env->CallVoidMethod(this->obj, this->showVirtualKeyboardMeth);
	}

	std::vector<std::string> listDirContents(const std::string& path){
		jstring p = this->env->NewStringUTF(path.c_str());
		jobject res = this->env->CallObjectMethod(this->obj, this->listDirContentsMeth, p);
		this->env->DeleteLocalRef(p);

		utki::ScopeExit scopeExit([this, res](){
			this->env->DeleteLocalRef(res);
		});

		std::vector<std::string> ret;

		if(res == nullptr){
			return ret;
		}

		jobjectArray arr = static_cast<jobjectArray>(res);

		int count = env->GetArrayLength(arr);

		for (int i = 0; i < count; ++i) {
			jstring str = static_cast<jstring>(env->GetObjectArrayElement(arr, i));
			auto chars = env->GetStringUTFChars(str, nullptr);
			ret.push_back(std::string(chars));
			this->env->ReleaseStringUTFChars(str, chars);
			this->env->DeleteLocalRef(str);
		}

		return ret;
	}

	std::string getStorageDir(){
		jobject res = this->env->CallObjectMethod(this->obj, this->getStorageDirMeth);
		utki::ScopeExit resScopeExit([this, &res](){
			this->env->DeleteLocalRef(res);
		});

		jstring str = static_cast<jstring>(res);

		auto chars = env->GetStringUTFChars(str, nullptr);
		utki::ScopeExit charsScopeExit([this, &chars, &str](){
			this->env->ReleaseStringUTFChars(str, chars);
		});

		return std::string(chars);
	}
};


std::unique_ptr<JavaFunctionsWrapper> javaFunctionsWrapper;

struct WindowWrapper : public utki::Unique{
	EGLDisplay display;
	EGLSurface surface;
	EGLContext context;

	nitki::Queue uiQueue;

	WindowWrapper(const window_params& wp){
		this->display = eglGetDisplay(EGL_DEFAULT_DISPLAY);
		if(this->display == EGL_NO_DISPLAY){
			throw morda::Exc("eglGetDisplay(): failed, no matching display connection found");
		}

		utki::ScopeExit eglDisplayScopeExit([this](){
			eglTerminate(this->display);
		});

		if(eglInitialize(this->display, 0, 0) == EGL_FALSE){
			throw morda::Exc("eglInitialize() failed");
		}

		//TODO: allow stencil configuration etc. via window_params
		//Here specify the attributes of the desired configuration.
		//Below, we select an EGLConfig with at least 8 bits per color
		//component compatible with on-screen windows
		const EGLint attribs[] = {
				EGL_SURFACE_TYPE, EGL_WINDOW_BIT,
				EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT, //we want OpenGL ES 2.0
				EGL_BLUE_SIZE, 8,
				EGL_GREEN_SIZE, 8,
				EGL_RED_SIZE, 8,
				EGL_NONE
		};

		EGLConfig config;

		//Here, the application chooses the configuration it desires. In this
		//sample, we have a very simplified selection process, where we pick
		//the first EGLConfig that matches our criteria
		EGLint numConfigs;
		eglChooseConfig(this->display, attribs, &config, 1, &numConfigs);
		if(numConfigs <= 0){
			throw morda::Exc("eglChooseConfig() failed, no matching config found");
		}

		//EGL_NATIVE_VISUAL_ID is an attribute of the EGLConfig that is
		//guaranteed to be accepted by ANativeWindow_setBuffersGeometry().
		//As soon as we picked a EGLConfig, we can safely reconfigure the
		//ANativeWindow buffers to match, using EGL_NATIVE_VISUAL_ID.
		EGLint format;
		if(eglGetConfigAttrib(this->display, config, EGL_NATIVE_VISUAL_ID, &format) == EGL_FALSE){
			throw morda::Exc("eglGetConfigAttrib() failed");
		}

		ASSERT(androidWindow)
		ANativeWindow_setBuffersGeometry(androidWindow, 0, 0, format);

		this->surface = eglCreateWindowSurface(this->display, config, androidWindow, NULL);
		if(this->surface == EGL_NO_SURFACE){
			throw morda::Exc("eglCreateWindowSurface() failed");
		}

		utki::ScopeExit eglSurfaceScopeExit([this](){
			eglDestroySurface(this->display, this->surface);
		});


		EGLint contextAttrs[] = {
				EGL_CONTEXT_CLIENT_VERSION, 2, //This is needed on Android, otherwise eglCreateContext() thinks that we want OpenGL ES 1.1, but we want 2.0
				EGL_NONE
		};

		this->context = eglCreateContext(this->display, config, NULL, contextAttrs);
		if(this->context == EGL_NO_CONTEXT){
			throw morda::Exc("eglCreateContext() failed");
		}

		utki::ScopeExit eglContextScopeExit([this](){
			eglDestroyContext(this->display, this->context);
		});

		if(eglMakeCurrent(this->display, this->surface, this->surface, this->context) == EGL_FALSE){
			throw morda::Exc("eglMakeCurrent() failed");
		}

		eglContextScopeExit.reset();
		eglSurfaceScopeExit.reset();
		eglDisplayScopeExit.reset();
	}

	r4::vec2ui getWindowSize(){
		EGLint width, height;
		eglQuerySurface(this->display, this->surface, EGL_WIDTH, &width);
		eglQuerySurface(this->display, this->surface, EGL_HEIGHT, &height);
		return r4::vec2ui(width, height);
	}

	void swapBuffers(){
		eglSwapBuffers(this->display, this->surface);
	}

	~WindowWrapper()noexcept{
		eglMakeCurrent(this->display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
		eglDestroyContext(this->display, this->context);
		eglDestroySurface(this->display, this->surface);
		eglTerminate(this->display);
	}
};

WindowWrapper& getImpl(const std::unique_ptr<utki::Unique>& pimpl){
	ASSERT(dynamic_cast<WindowWrapper*>(pimpl.get()))
	return static_cast<WindowWrapper&>(*pimpl);
}


class AssetFile : public papki::File{
	AAssetManager* manager;

	mutable AAsset* handle = nullptr;

public:

	AssetFile(AAssetManager* manager, const std::string& pathName = std::string()) :
			manager(manager),
			File(pathName)
	{
		ASSERT(this->manager)
	}


	virtual void openInternal(E_Mode mode)override{
		switch(mode){
			case papki::File::E_Mode::WRITE:
			case papki::File::E_Mode::CREATE:
				throw papki::Exc("WRITE and CREATE open modes are not supported by Android assets");
				break;
			case papki::File::E_Mode::READ:
				break;
			default:
				throw papki::Exc("unknown mode");
				break;
		}
		this->handle = AAssetManager_open(this->manager, this->path().c_str(), AASSET_MODE_UNKNOWN); //don't know what this MODE means at all
		if(!this->handle){
			std::stringstream ss;
			ss << "AAssetManager_open(" << this->path() << ") failed";
			throw papki::Exc(ss.str());
		}
	}

	virtual void closeInternal()const noexcept override{
		ASSERT(this->handle)
		AAsset_close(this->handle);
		this->handle = 0;
	}

	virtual size_t readInternal(utki::Buf<std::uint8_t> buf)const override{
		ASSERT(this->handle)
		int numBytesRead = AAsset_read(this->handle, &*buf.begin(), buf.size());
		if(numBytesRead < 0){//something happened
			throw papki::Exc("AAsset_read() error");
		}
		ASSERT(numBytesRead >= 0)
		return size_t(numBytesRead);
	}

	virtual size_t writeInternal(const utki::Buf<std::uint8_t> buf)override{
		ASSERT(this->handle)
		throw papki::Exc("Write() is not supported by Android assets");
		return 0;
	}

	virtual size_t seekForwardInternal(size_t numBytesToSeek)const override{
		return this->seek(numBytesToSeek, true);
	}

	virtual size_t seekBackwardInternal(size_t numBytesToSeek)const override{
		return this->seek(numBytesToSeek, false);
	}

	virtual void rewindInternal()const override{
		if(!this->isOpened()){
			throw papki::Exc("file is not opened, cannot rewind");
		}

		ASSERT(this->handle)
		if(AAsset_seek(this->handle, 0, SEEK_SET) < 0){
			throw papki::Exc("AAsset_seek() failed");
		}
	}

	virtual bool exists()const override{
		if(this->isOpened()){ //file is opened => it exists
			return true;
		}

		if(this->path().size() == 0){
			return false;
		}

		if(this->isDir()){
			//try opening the directory to check its existence
			AAssetDir* pdir = AAssetManager_openDir(this->manager, this->path().c_str());

			if(!pdir){
				return false;
			}else{
				AAssetDir_close(pdir);
				return true;
			}
		}else{
			return this->File::exists();
		}
	}

	virtual std::vector<std::string> listDirContents(size_t maxEntries = 0)const override{
		if(!this->isDir()){
			throw papki::IllegalStateExc("AndroidAssetFile::ListDirContents(): this is not a directory");
		}

		//Trim away trailing '/', as Android does not work with it.
		auto p = this->path().substr(0, this->path().size() - 1);

		ASSERT(javaFunctionsWrapper)
		return javaFunctionsWrapper->listDirContents(p);
	}

	std::unique_ptr<papki::File> spawn()override{
		return utki::makeUnique<AssetFile>(this->manager);
	}

	~AssetFile()noexcept{
	}

	size_t seek(size_t numBytesToSeek, bool seekForward)const{
		if(!this->isOpened()){
			throw papki::Exc("file is not opened, cannot seek");
		}

		ASSERT(this->handle)

		//NOTE: AAsset_seek() accepts 'off_t' as offset argument which is signed and can be
		//      less than size_t value passed as argument to this function.
		//      Therefore, do several seek operations with smaller offset if necessary.

		off_t assetSize = AAsset_getLength(this->handle);
		ASSERT(assetSize >= 0)

		if(seekForward){
			ASSERT(size_t(assetSize) >= this->curPos())
			utki::clampTop(numBytesToSeek, size_t(assetSize) - this->curPos());
		}else{
			utki::clampTop(numBytesToSeek, this->curPos());
		}

		typedef off_t T_FSeekOffset;
		const size_t DMax = ((size_t(T_FSeekOffset(-1))) >> 1);
		ASSERT((size_t(1) << ((sizeof(T_FSeekOffset) * 8) - 1)) - 1 == DMax)
		static_assert(size_t(-(-T_FSeekOffset(DMax))) == DMax, "size mismatch");

		for(size_t numBytesLeft = numBytesToSeek; numBytesLeft != 0;){
			ASSERT(numBytesLeft <= numBytesToSeek)

			T_FSeekOffset offset;
			if(numBytesLeft > DMax){
				offset = T_FSeekOffset(DMax);
			}else{
				offset = T_FSeekOffset(numBytesLeft);
			}

			ASSERT(offset > 0)

			if(AAsset_seek(this->handle, seekForward ? offset : (-offset), SEEK_CUR) < 0){
				throw papki::Exc("AAsset_seek() failed");
			}

			ASSERT(size_t(offset) < size_t(-1))
			ASSERT(numBytesLeft >= size_t(offset))

			numBytesLeft -= size_t(offset);
		}
		return numBytesToSeek;
	}
};



morda::Vec2r curWinDim(0, 0);

AInputQueue* curInputQueue = 0;

struct AppInfo{
	//Path to this application's internal data directory.
	const char* internalDataPath;

	//Path to this application's external (removable/mountable) data directory.
	const char* externalDataPath;

	//Pointer to the Asset Manager instance for the application. The application
	//uses this to access binary assets bundled inside its own .apk file.
	AAssetManager* assetManager;
} appInfo;



//array of current pointer positions, needed to detect which pointers have actually moved.
std::array<morda::Vec2r, 10> pointers;



inline morda::Vec2r AndroidWinCoordsToMordaWinRectCoords(const morda::Vec2r& winDim, const morda::Vec2r& p){
	morda::Vec2r ret(
			p.x,
			p.y - (curWinDim.y - winDim.y)
		);
//	TRACE(<< "AndroidWinCoordsToMordaWinRectCoords(): ret = " << ret << std::endl)
	return ret.rounded();
}



struct AndroidConfiguration{
	AConfiguration* ac;

	AndroidConfiguration(){
		this->ac = AConfiguration_new();
	}

	~AndroidConfiguration()noexcept{
		AConfiguration_delete(this->ac);
	}

	static inline std::unique_ptr<AndroidConfiguration> New(){
		return std::unique_ptr<AndroidConfiguration>(new AndroidConfiguration());
	}
};

std::unique_ptr<AndroidConfiguration> curConfig;



class KeyEventToUnicodeResolver : public morda::Morda::UnicodeProvider{
public:
	int32_t kc;//key code
	int32_t ms;//meta state
	int32_t di;//device id

	std::u32string get()const{
		ASSERT(javaFunctionsWrapper)
//		TRACE(<< "KeyEventToUnicodeResolver::Resolve(): this->kc = " << this->kc << std::endl)
		char32_t res = javaFunctionsWrapper->resolveKeyUnicode(this->di, this->ms, this->kc);

		//0 means that key did not produce any unicode character
		if(res == 0){
			TRACE(<< "key did not produce any unicode character, returning empty string" << std::endl)
			return std::u32string();
		}

		return std::u32string(&res, 1);
	}


} keyUnicodeResolver;



//================
// for Updatable
//================

class FDFlag{
	int eventFD;
public:
	FDFlag(){
		this->eventFD = eventfd(0, EFD_NONBLOCK);
		if(this->eventFD < 0){
			std::stringstream ss;
			ss << "FDFlag::FDFlag(): could not create eventFD (*nix) for implementing Waitable,"
					<< " error code = " << errno << ": " << strerror(errno);
			throw utki::Exc(ss.str().c_str());
		}
	}

	~FDFlag()noexcept{
		close(this->eventFD);
	}

	int GetFD()noexcept{
		return this->eventFD;
	}

	void Set(){
		if(eventfd_write(this->eventFD, 1) < 0){
			ASSERT(false)
		}
	}

	void Clear(){
		eventfd_t value;
		if(eventfd_read(this->eventFD, &value) < 0){
			if(errno == EAGAIN){
				return;
			}
			ASSERT(false)
		}
	}
} fdFlag;



class LinuxTimer{
	timer_t timer;

	//Handler for SIGALRM signal
	static void OnSIGALRM(int){
		fdFlag.Set();
	}

public:
	LinuxTimer(){
		int res = timer_create(
				CLOCK_MONOTONIC,
				0,//means SIGALRM signal is emitted when timer expires
				&this->timer
			);
		if(res != 0){
			throw morda::Exc("timer_create() failed");
		}

		struct sigaction sa;
		sa.sa_handler = &LinuxTimer::OnSIGALRM;
		sa.sa_flags = SA_NODEFER;
		memset(&sa.sa_mask, 0, sizeof(sa.sa_mask));

		res = sigaction(SIGALRM, &sa, 0);
		ASSERT(res == 0)
	}

	~LinuxTimer()noexcept{
		//set default handler for SIGALRM
		struct sigaction sa;
		sa.sa_handler = SIG_DFL;
		sa.sa_flags = 0;
		memset(&sa.sa_mask, 0, sizeof(sa.sa_mask));

#ifdef DEBUG
		int res =
#endif
		sigaction(SIGALRM, &sa, 0);
		ASSERT_INFO(res == 0, " res = " << res << " errno = " << errno)

		//delete timer
#ifdef DEBUG
		res =
#endif
		timer_delete(this->timer);
		ASSERT_INFO(res == 0, " res = " << res << " errno = " << errno)
	}

	//if timer is already armed, it will re-set the expiration time
	void Arm(std::uint32_t dt){
		itimerspec ts;
		ts.it_value.tv_sec = dt / 1000;
		ts.it_value.tv_nsec = (dt % 1000) * 1000000;
		ts.it_interval.tv_nsec = 0;//one shot timer
		ts.it_interval.tv_sec = 0;//one shot timer

#ifdef DEBUG
		int res =
#endif
		timer_settime(this->timer, 0, &ts, 0);
		ASSERT_INFO(res == 0, " res = " << res << " errno = " << errno)
	}

	//returns true if timer was disarmed
	//returns false if timer has fired before it was disarmed.
	bool Disarm(){
		itimerspec oldts;
		itimerspec newts;
		newts.it_value.tv_nsec = 0;
		newts.it_value.tv_sec = 0;

		int res = timer_settime(this->timer, 0, &newts, &oldts);
		if(res != 0){
			ASSERT_INFO(false, "errno = " << errno << " res = " << res)
		}

		if(oldts.it_value.tv_nsec != 0 || oldts.it_value.tv_sec != 0){
			return true;
		}
		return false;
	}
} timer;



//TODO: this mapping is not final
const std::array<morda::Key_e, std::uint8_t(-1) + 1> keyCodeMap = {
	morda::Key_e::UNKNOWN, //AKEYCODE_UNKNOWN
	morda::Key_e::LEFT, //AKEYCODE_SOFT_LEFT
	morda::Key_e::RIGHT, //AKEYCODE_SOFT_RIGHT
	morda::Key_e::HOME, //AKEYCODE_HOME
	morda::Key_e::ESCAPE, //AKEYCODE_BACK
	morda::Key_e::F11, //AKEYCODE_CALL
	morda::Key_e::F12, //AKEYCODE_ENDCALL
	morda::Key_e::ZERO, //AKEYCODE_0
	morda::Key_e::ONE, //AKEYCODE_1
	morda::Key_e::TWO, //AKEYCODE_2
	morda::Key_e::THREE, //AKEYCODE_3
	morda::Key_e::FOUR, //AKEYCODE_4
	morda::Key_e::FIVE, //AKEYCODE_5
	morda::Key_e::SIX, //AKEYCODE_6
	morda::Key_e::SEVEN, //AKEYCODE_7
	morda::Key_e::EIGHT, //AKEYCODE_8
	morda::Key_e::NINE, //AKEYCODE_9
	morda::Key_e::UNKNOWN, //AKEYCODE_STAR
	morda::Key_e::UNKNOWN, //AKEYCODE_POUND
	morda::Key_e::UP, //AKEYCODE_DPAD_UP
	morda::Key_e::DOWN, //AKEYCODE_DPAD_DOWN
	morda::Key_e::LEFT, //AKEYCODE_DPAD_LEFT
	morda::Key_e::RIGHT, //AKEYCODE_DPAD_RIGHT
	morda::Key_e::ENTER, //AKEYCODE_DPAD_CENTER
	morda::Key_e::PAGE_UP, //AKEYCODE_VOLUME_UP
	morda::Key_e::PAGE_DOWN, //AKEYCODE_VOLUME_DOWN
	morda::Key_e::F10, //AKEYCODE_POWER
	morda::Key_e::F9, //AKEYCODE_CAMERA
	morda::Key_e::BACKSPACE, //AKEYCODE_CLEAR
	morda::Key_e::A, //AKEYCODE_A
	morda::Key_e::B, //AKEYCODE_B
	morda::Key_e::C, //AKEYCODE_C
	morda::Key_e::D, //AKEYCODE_D
	morda::Key_e::E, //AKEYCODE_E
	morda::Key_e::F, //AKEYCODE_F
	morda::Key_e::G, //AKEYCODE_G
	morda::Key_e::H, //AKEYCODE_H
	morda::Key_e::I, //AKEYCODE_I
	morda::Key_e::G, //AKEYCODE_J
	morda::Key_e::K, //AKEYCODE_K
	morda::Key_e::L, //AKEYCODE_L
	morda::Key_e::M, //AKEYCODE_M
	morda::Key_e::N, //AKEYCODE_N
	morda::Key_e::O, //AKEYCODE_O
	morda::Key_e::P, //AKEYCODE_P
	morda::Key_e::Q, //AKEYCODE_Q
	morda::Key_e::R, //AKEYCODE_R
	morda::Key_e::S, //AKEYCODE_S
	morda::Key_e::T, //AKEYCODE_T
	morda::Key_e::U, //AKEYCODE_U
	morda::Key_e::V, //AKEYCODE_V
	morda::Key_e::W, //AKEYCODE_W
	morda::Key_e::X, //AKEYCODE_X
	morda::Key_e::Y, //AKEYCODE_Y
	morda::Key_e::Z, //AKEYCODE_Z
	morda::Key_e::V, //AKEYCODE_COMMA
	morda::Key_e::B, //AKEYCODE_PERIOD
	morda::Key_e::N, //AKEYCODE_ALT_LEFT
	morda::Key_e::M, //AKEYCODE_ALT_RIGHT
	morda::Key_e::LEFT_SHIFT, //AKEYCODE_SHIFT_LEFT
	morda::Key_e::RIGHT_SHIFT, //AKEYCODE_SHIFT_RIGHT
	morda::Key_e::TAB, //AKEYCODE_TAB
	morda::Key_e::SPACE, //AKEYCODE_SPACE
	morda::Key_e::LEFT_CONTROL, //AKEYCODE_SYM
	morda::Key_e::F8, //AKEYCODE_EXPLORER
	morda::Key_e::F7, //AKEYCODE_ENVELOPE
	morda::Key_e::ENTER, //AKEYCODE_ENTER
	morda::Key_e::DELETE, //AKEYCODE_DEL
	morda::Key_e::F6, //AKEYCODE_GRAVE
	morda::Key_e::MINUS, //AKEYCODE_MINUS
	morda::Key_e::EQUALS, //AKEYCODE_EQUALS
	morda::Key_e::LEFT_SQUARE_BRACKET, //AKEYCODE_LEFT_BRACKET
	morda::Key_e::RIGHT_SQUARE_BRACKET, //AKEYCODE_RIGHT_BRACKET
	morda::Key_e::BACKSLASH, //AKEYCODE_BACKSLASH
	morda::Key_e::SEMICOLON, //AKEYCODE_SEMICOLON
	morda::Key_e::APOSTROPHE, //AKEYCODE_APOSTROPHE
	morda::Key_e::SLASH, //AKEYCODE_SLASH
	morda::Key_e::GRAVE, //AKEYCODE_AT
	morda::Key_e::F5, //AKEYCODE_NUM
	morda::Key_e::F4, //AKEYCODE_HEADSETHOOK
	morda::Key_e::F3, //AKEYCODE_FOCUS (camera focus)
	morda::Key_e::F2, //AKEYCODE_PLUS
	morda::Key_e::F1, //AKEYCODE_MENU
	morda::Key_e::END, //AKEYCODE_NOTIFICATION
	morda::Key_e::RIGHT_CONTROL, //AKEYCODE_SEARCH
	morda::Key_e::UNKNOWN, //AKEYCODE_MEDIA_PLAY_PAUSE
	morda::Key_e::UNKNOWN, //AKEYCODE_MEDIA_STOP
	morda::Key_e::UNKNOWN, //AKEYCODE_MEDIA_NEXT
	morda::Key_e::UNKNOWN, //AKEYCODE_MEDIA_PREVIOUS
	morda::Key_e::UNKNOWN, //AKEYCODE_MEDIA_REWIND
	morda::Key_e::UNKNOWN, //AKEYCODE_MEDIA_FAST_FORWARD
	morda::Key_e::UNKNOWN, //AKEYCODE_MUTE
	morda::Key_e::PAGE_UP, //AKEYCODE_PAGE_UP
	morda::Key_e::PAGE_DOWN, //AKEYCODE_PAGE_DOWN
	morda::Key_e::UNKNOWN, //AKEYCODE_PICTSYMBOLS
	morda::Key_e::CAPSLOCK, //AKEYCODE_SWITCH_CHARSET
	morda::Key_e::UNKNOWN, //AKEYCODE_BUTTON_A
	morda::Key_e::UNKNOWN, //AKEYCODE_BUTTON_B
	morda::Key_e::UNKNOWN, //AKEYCODE_BUTTON_C
	morda::Key_e::UNKNOWN, //AKEYCODE_BUTTON_X
	morda::Key_e::UNKNOWN, //AKEYCODE_BUTTON_Y
	morda::Key_e::UNKNOWN, //AKEYCODE_BUTTON_Z
	morda::Key_e::UNKNOWN, //AKEYCODE_BUTTON_L1
	morda::Key_e::UNKNOWN, //AKEYCODE_BUTTON_R1
	morda::Key_e::UNKNOWN, //AKEYCODE_BUTTON_L2
	morda::Key_e::UNKNOWN, //AKEYCODE_BUTTON_R2
	morda::Key_e::UNKNOWN, //AKEYCODE_BUTTON_THUMBL
	morda::Key_e::UNKNOWN, //AKEYCODE_BUTTON_THUMBR
	morda::Key_e::UNKNOWN, //AKEYCODE_BUTTON_START
	morda::Key_e::UNKNOWN, //AKEYCODE_BUTTON_SELECT
	morda::Key_e::UNKNOWN, //AKEYCODE_BUTTON_MODE
	morda::Key_e::UNKNOWN, //
	morda::Key_e::UNKNOWN, //
	morda::Key_e::UNKNOWN, //
	morda::Key_e::UNKNOWN, //
	morda::Key_e::UNKNOWN, //
	morda::Key_e::UNKNOWN, //
	morda::Key_e::UNKNOWN, //
	morda::Key_e::UNKNOWN, //
	morda::Key_e::UNKNOWN, //
	morda::Key_e::UNKNOWN, //
	morda::Key_e::UNKNOWN, //
	morda::Key_e::UNKNOWN, //
	morda::Key_e::UNKNOWN, //
	morda::Key_e::UNKNOWN, //
	morda::Key_e::UNKNOWN, //
	morda::Key_e::UNKNOWN, //
	morda::Key_e::UNKNOWN, //
	morda::Key_e::UNKNOWN, //
	morda::Key_e::UNKNOWN, //
	morda::Key_e::UNKNOWN, //
	morda::Key_e::UNKNOWN, //
	morda::Key_e::UNKNOWN, //
	morda::Key_e::UNKNOWN, //
	morda::Key_e::UNKNOWN, //
	morda::Key_e::UNKNOWN, //
	morda::Key_e::UNKNOWN, //
	morda::Key_e::UNKNOWN, //
	morda::Key_e::UNKNOWN, //
	morda::Key_e::UNKNOWN, //
	morda::Key_e::UNKNOWN, //
	morda::Key_e::UNKNOWN, //
	morda::Key_e::UNKNOWN, //
	morda::Key_e::UNKNOWN, //
	morda::Key_e::UNKNOWN, //
	morda::Key_e::UNKNOWN, //
	morda::Key_e::UNKNOWN, //
	morda::Key_e::UNKNOWN, //
	morda::Key_e::UNKNOWN, //
	morda::Key_e::UNKNOWN, //
	morda::Key_e::UNKNOWN, //
	morda::Key_e::UNKNOWN, //
	morda::Key_e::UNKNOWN, //
	morda::Key_e::UNKNOWN, //
	morda::Key_e::UNKNOWN, //
	morda::Key_e::UNKNOWN, //
	morda::Key_e::UNKNOWN, //
	morda::Key_e::UNKNOWN, //
	morda::Key_e::UNKNOWN, //
	morda::Key_e::UNKNOWN, //
	morda::Key_e::UNKNOWN, //
	morda::Key_e::UNKNOWN, //
	morda::Key_e::UNKNOWN, //
	morda::Key_e::UNKNOWN, //
	morda::Key_e::UNKNOWN, //
	morda::Key_e::UNKNOWN, //
	morda::Key_e::UNKNOWN, //
	morda::Key_e::UNKNOWN, //
	morda::Key_e::UNKNOWN, //
	morda::Key_e::UNKNOWN, //
	morda::Key_e::UNKNOWN, //
	morda::Key_e::UNKNOWN, //
	morda::Key_e::UNKNOWN, //
	morda::Key_e::UNKNOWN, //
	morda::Key_e::UNKNOWN, //
	morda::Key_e::UNKNOWN, //
	morda::Key_e::UNKNOWN, //
	morda::Key_e::UNKNOWN, //
	morda::Key_e::UNKNOWN, //
	morda::Key_e::UNKNOWN, //
	morda::Key_e::UNKNOWN, //
	morda::Key_e::UNKNOWN, //
	morda::Key_e::UNKNOWN, //
	morda::Key_e::UNKNOWN, //
	morda::Key_e::UNKNOWN, //
	morda::Key_e::UNKNOWN, //
	morda::Key_e::UNKNOWN, //
	morda::Key_e::UNKNOWN, //
	morda::Key_e::UNKNOWN, //
	morda::Key_e::UNKNOWN, //
	morda::Key_e::UNKNOWN, //
	morda::Key_e::UNKNOWN, //
	morda::Key_e::UNKNOWN, //
	morda::Key_e::UNKNOWN, //
	morda::Key_e::UNKNOWN, //
	morda::Key_e::UNKNOWN, //
	morda::Key_e::UNKNOWN, //
	morda::Key_e::UNKNOWN, //
	morda::Key_e::UNKNOWN, //
	morda::Key_e::UNKNOWN, //
	morda::Key_e::UNKNOWN, //
	morda::Key_e::UNKNOWN, //
	morda::Key_e::UNKNOWN, //
	morda::Key_e::UNKNOWN, //
	morda::Key_e::UNKNOWN, //
	morda::Key_e::UNKNOWN, //
	morda::Key_e::UNKNOWN, //
	morda::Key_e::UNKNOWN, //
	morda::Key_e::UNKNOWN, //
	morda::Key_e::UNKNOWN, //
	morda::Key_e::UNKNOWN, //
	morda::Key_e::UNKNOWN, //
	morda::Key_e::UNKNOWN, //
	morda::Key_e::UNKNOWN, //
	morda::Key_e::UNKNOWN, //
	morda::Key_e::UNKNOWN, //
	morda::Key_e::UNKNOWN, //
	morda::Key_e::UNKNOWN, //
	morda::Key_e::UNKNOWN, //
	morda::Key_e::UNKNOWN, //
	morda::Key_e::UNKNOWN, //
	morda::Key_e::UNKNOWN, //
	morda::Key_e::UNKNOWN, //
	morda::Key_e::UNKNOWN, //
	morda::Key_e::UNKNOWN, //
	morda::Key_e::UNKNOWN, //
	morda::Key_e::UNKNOWN, //
	morda::Key_e::UNKNOWN, //
	morda::Key_e::UNKNOWN, //
	morda::Key_e::UNKNOWN, //
	morda::Key_e::UNKNOWN, //
	morda::Key_e::UNKNOWN, //
	morda::Key_e::UNKNOWN, //
	morda::Key_e::UNKNOWN, //
	morda::Key_e::UNKNOWN, //
	morda::Key_e::UNKNOWN, //
	morda::Key_e::UNKNOWN, //
	morda::Key_e::UNKNOWN, //
	morda::Key_e::UNKNOWN, //
	morda::Key_e::UNKNOWN, //
	morda::Key_e::UNKNOWN, //
	morda::Key_e::UNKNOWN, //
	morda::Key_e::UNKNOWN, //
	morda::Key_e::UNKNOWN, //
	morda::Key_e::UNKNOWN, //
	morda::Key_e::UNKNOWN, //
	morda::Key_e::UNKNOWN, //
	morda::Key_e::UNKNOWN, //
	morda::Key_e::UNKNOWN, //
	morda::Key_e::UNKNOWN, //
	morda::Key_e::UNKNOWN, //
	morda::Key_e::UNKNOWN, //
	morda::Key_e::UNKNOWN, //
	morda::Key_e::UNKNOWN, //
	morda::Key_e::UNKNOWN, //
	morda::Key_e::UNKNOWN  //
};



morda::Key_e getKeyFromKeyEvent(AInputEvent& event)noexcept{
	size_t kc = size_t(AKeyEvent_getKeyCode(&event));
	ASSERT(kc < keyCodeMap.size())
	return keyCodeMap[kc];
}



struct UnicodeProvider : public morda::Morda::UnicodeProvider{
	std::u32string chars;

	std::u32string get()const override{
		return std::move(this->chars);
	}
};



}//~namespace





namespace{



JNIEXPORT void JNICALL Java_io_github_igagis_mordavokne_MordaVOkneActivity_handleCharacterStringInput(
		JNIEnv *env,
		jclass clazz,
		jstring chars
	)
{
	TRACE(<< "handleCharacterStringInput(): invoked" << std::endl)

	const char *utf8Chars = env->GetStringUTFChars(chars, 0);

	utki::ScopeExit scopeExit([env, &chars, utf8Chars](){
		env->ReleaseStringUTFChars(chars, utf8Chars);
	});

	if(utf8Chars == nullptr || *utf8Chars == 0){
		TRACE(<< "handleCharacterStringInput(): empty string passed in" << std::endl)
		return;
	}

	TRACE(<< "handleCharacterStringInput(): utf8Chars = " << utf8Chars << std::endl)

	std::vector<char32_t> utf32;

	for(unikod::Utf8Iterator i(utf8Chars); !i.isEnd(); ++i){
		utf32.push_back(i.character());
	}

	UnicodeProvider resolver;
	resolver.chars = std::u32string(&*utf32.begin(), utf32.size());

//    TRACE(<< "handleCharacterStringInput(): resolver.chars = " << resolver.chars << std::endl)

	mordavokne::handleCharacterInput(mordavokne::inst(), resolver, morda::Key_e::UNKNOWN);
}



}//~namespace



jint JNI_OnLoad(JavaVM* vm, void* reserved){
	TRACE(<< "JNI_OnLoad(): invoked" << std::endl)

	JNIEnv* env;
	if (vm->GetEnv(reinterpret_cast<void**>(&env), JNI_VERSION_1_6) != JNI_OK) {
		return -1;
	}

	static JNINativeMethod methods[] = {
		{"handleCharacterStringInput", "(Ljava/lang/String;)V", (void*)&Java_io_github_igagis_mordavokne_MordaVOkneActivity_handleCharacterStringInput},
	};
	jclass clazz = env->FindClass("io/github/igagis/mordavokne/MordaVOkneActivity");
	ASSERT(clazz)
	if(env->RegisterNatives(clazz, methods, 1) < 0){
		ASSERT(false)
	}

	return JNI_VERSION_1_6;
}

namespace{
std::string initializeStorageDir(const std::string& appName){
	ASSERT(javaFunctionsWrapper)

	auto dir = javaFunctionsWrapper->getStorageDir();

	if(*dir.rend() != '/'){
		dir.append(1, '/');
	}
	return dir;
}
}

mordavokne::application::application(std::string&& name, const window_params& requestedWindowParams) :
		name(name),
		windowPimpl(utki::makeUnique<WindowWrapper>(requestedWindowParams)),
		gui(
				std::make_shared<mordaren::OpenGLES2Renderer>(),
				[]() -> float{
					ASSERT(javaFunctionsWrapper)

					return javaFunctionsWrapper->getDotsPerInch();
				}(),
				[this]() -> float{
					auto res = getImpl(this->windowPimpl).getWindowSize();
					auto dim = (res.to<float>() / javaFunctionsWrapper->getDotsPerInch()) * 25.4f;
					return application::findDotsPerDp(res, dim.to<unsigned>());
				}(),
				[this](std::function<void()>&& a){
					getImpl(getWindowPimpl(*this)).uiQueue.pushMessage(std::move(a));
				}
			),
		storage_dir(initializeStorageDir(this->name))
{
	auto winSize = getImpl(this->windowPimpl).getWindowSize();
	this->updateWindowRect(morda::Rectr(morda::Vec2r(0), winSize.to<morda::real>()));
}

std::unique_ptr<papki::File> mordavokne::application::get_res_file(const std::string& path)const{
	return utki::makeUnique<AssetFile>(appInfo.assetManager, path);
}

void mordavokne::application::swapFrameBuffers() {
	auto& ww = getImpl(this->windowPimpl);
	ww.swapBuffers();
}

void mordavokne::application::setMouseCursorVisible(bool visible) {
	//do nothing
}

void mordavokne::application::set_fullscreen(bool enable) {
	ASSERT(nativeActivity)
	if(enable) {
		ANativeActivity_setWindowFlags(nativeActivity, AWINDOW_FLAG_FULLSCREEN, 0);
	}else{
		ANativeActivity_setWindowFlags(nativeActivity, 0, AWINDOW_FLAG_FULLSCREEN);
	}
}

void mordavokne::application::quit()noexcept{
	ASSERT(nativeActivity)
	ANativeActivity_finish(nativeActivity);
}

void mordavokne::application::show_virtual_keyboard()noexcept{
	//NOTE:
	//ANativeActivity_showSoftInput(nativeActivity, ANATIVEACTIVITY_SHOW_SOFT_INPUT_FORCED);
	//did not work for some reason.

	ASSERT(javaFunctionsWrapper)
	javaFunctionsWrapper->show_virtual_keyboard();
}



void mordavokne::application::hide_virtual_keyboard()noexcept{
	//NOTE:
	//ANativeActivity_hideSoftInput(nativeActivity, ANATIVEACTIVITY_HIDE_SOFT_INPUT_NOT_ALWAYS);
	//did not work for some reason

	ASSERT(javaFunctionsWrapper)
	javaFunctionsWrapper->hide_virtual_keyboard();
}



namespace{
void handleInputEvents(){
	auto& app = mordavokne::inst();

	//Read and handle input events
	AInputEvent* event;
	while(AInputQueue_getEvent(curInputQueue, &event) >= 0){
		ASSERT(event)

//		TRACE(<< "New input event: type = " << AInputEvent_getType(event) << std::endl)
		if(AInputQueue_preDispatchEvent(curInputQueue, event)){
			continue;
		}

		int32_t eventType = AInputEvent_getType(event);
		int32_t eventAction = AMotionEvent_getAction(event);

		bool consume = false;

		switch(eventType){
			case AINPUT_EVENT_TYPE_MOTION:
				switch(eventAction & AMOTION_EVENT_ACTION_MASK){
					case AMOTION_EVENT_ACTION_POINTER_DOWN:
//						TRACE(<< "Pointer down" << std::endl)
					case AMOTION_EVENT_ACTION_DOWN:
					{
						unsigned pointerIndex = ((eventAction & AMOTION_EVENT_ACTION_POINTER_INDEX_MASK) >> AMOTION_EVENT_ACTION_POINTER_INDEX_SHIFT);
						unsigned pointerId = unsigned(AMotionEvent_getPointerId(event, pointerIndex));

						if(pointerId >= pointers.size()){
							TRACE(<< "Pointer ID is too big, only " << pointers.size() << " pointers supported at maximum")
							continue;
						}

//								TRACE(<< "Action down, ptr id = " << pointerId << std::endl)

						morda::Vec2r p(AMotionEvent_getX(event, pointerIndex), AMotionEvent_getY(event, pointerIndex));
						pointers[pointerId] = p;

						handleMouseButton(
								app,
								true,
								AndroidWinCoordsToMordaWinRectCoords(app.window_dimensions(), p),
								morda::MouseButton_e::LEFT,
								pointerId
						);
					}
						break;
					case AMOTION_EVENT_ACTION_POINTER_UP:
//						TRACE(<< "Pointer up" << std::endl)
					case AMOTION_EVENT_ACTION_UP:
					{
						unsigned pointerIndex = ((eventAction & AMOTION_EVENT_ACTION_POINTER_INDEX_MASK) >> AMOTION_EVENT_ACTION_POINTER_INDEX_SHIFT);
						unsigned pointerId = unsigned(AMotionEvent_getPointerId(event, pointerIndex));

						if(pointerId >= pointers.size()){
							TRACE(<< "Pointer ID is too big, only " << pointers.size() << " pointers supported at maximum")
							continue;
						}

//								TRACE(<< "Action up, ptr id = " << pointerId << std::endl)

						morda::Vec2r p(AMotionEvent_getX(event, pointerIndex), AMotionEvent_getY(event, pointerIndex));
						pointers[pointerId] = p;

						handleMouseButton(
								app,
								false,
								AndroidWinCoordsToMordaWinRectCoords(app.window_dimensions(), p),
								morda::MouseButton_e::LEFT,
								pointerId
						);
					}
						break;
					case AMOTION_EVENT_ACTION_MOVE:
					{
						size_t numPointers = AMotionEvent_getPointerCount(event);
						ASSERT(numPointers >= 1)
						for(size_t pointerNum = 0; pointerNum < numPointers; ++pointerNum){
							unsigned pointerId = unsigned(AMotionEvent_getPointerId(event, pointerNum));
							if(pointerId >= pointers.size()){
								TRACE(<< "Pointer ID is too big, only " << pointers.size() << " pointers supported at maximum")
								continue;
							}

							//notify root Container only if there was actual movement
							morda::Vec2r p(AMotionEvent_getX(event, pointerNum), AMotionEvent_getY(event, pointerNum));
							if(pointers[pointerId] == p){
								//pointer position did not change
								continue;
							}

//								TRACE(<< "Action move, ptr id = " << pointerId << std::endl)

							pointers[pointerId] = p;

							handleMouseMove(
									app,
									AndroidWinCoordsToMordaWinRectCoords(app.window_dimensions(), p),
									pointerId
							);
						}//~for(every pointer)
					}
						break;
					default:
						TRACE(<< "unknown eventAction = " << eventAction << std::endl)
						break;
				}//~switch(event action)
				consume = true;
				break;
			case AINPUT_EVENT_TYPE_KEY:
				{
//					TRACE(<< "AINPUT_EVENT_TYPE_KEY" << std::endl)

					ASSERT(event)
					morda::Key_e key = getKeyFromKeyEvent(*event);

					keyUnicodeResolver.kc = AKeyEvent_getKeyCode(event);
					keyUnicodeResolver.ms = AKeyEvent_getMetaState(event);
					keyUnicodeResolver.di = AInputEvent_getDeviceId(event);

//    				TRACE(<< "AINPUT_EVENT_TYPE_KEY: keyUnicodeResolver.kc = " << keyUnicodeResolver.kc << std::endl)

					switch(eventAction){
						case AKEY_EVENT_ACTION_DOWN:
//    						TRACE(<< "AKEY_EVENT_ACTION_DOWN, count = " << AKeyEvent_getRepeatCount(event) << std::endl)
							//detect auto-repeated key events
							if(AKeyEvent_getRepeatCount(event) == 0){
								handleKeyEvent(app, true, key);
							}
							handleCharacterInput(app, keyUnicodeResolver, key);
							break;
						case AKEY_EVENT_ACTION_UP:
//    						TRACE(<< "AKEY_EVENT_ACTION_UP" << std::endl)
							handleKeyEvent(app, false, key);
							break;
						case AKEY_EVENT_ACTION_MULTIPLE:
//                          TRACE(<< "AKEY_EVENT_ACTION_MULTIPLE"
//                                  << " count = " << AKeyEvent_getRepeatCount(event)
//                                  << " keyCode = " << AKeyEvent_getKeyCode(event)
//                                  << std::endl)

							//Ignore, it is handled on Java side.

							break;
						default:
							TRACE(<< "unknown AINPUT_EVENT_TYPE_KEY eventAction: " << eventAction << std::endl)
							break;
					}
				}
				break;
			default:
				break;
		}//~switch(event type)

		AInputQueue_finishEvent(
				curInputQueue,
				event,
				consume
		);
	}//~while(there are events in input queue)

	render(app);

	fdFlag.Set();
}
}

namespace{
void OnDestroy(ANativeActivity* activity){
	TRACE(<< "OnDestroy(): invoked" << std::endl)

	javaFunctionsWrapper.reset();
}



void OnStart(ANativeActivity* activity){
	TRACE(<< "OnStart(): invoked" << std::endl)
}



void OnResume(ANativeActivity* activity){
	TRACE(<< "OnResume(): invoked" << std::endl)
}

void* OnSaveInstanceState(ANativeActivity* activity, size_t* outSize){
	TRACE(<< "OnSaveInstanceState(): invoked" << std::endl)

	//Do nothing, we don't use this mechanism of saving state.

	return nullptr;
}



void OnPause(ANativeActivity* activity){
	TRACE(<< "OnPause(): invoked" << std::endl)
}



void OnStop(ANativeActivity* activity){
	TRACE(<< "OnStop(): invoked" << std::endl)
}



void OnConfigurationChanged(ANativeActivity* activity){
	TRACE(<< "OnConfigurationChanged(): invoked" << std::endl)

	int32_t diff;
	{
		std::unique_ptr<AndroidConfiguration> config = AndroidConfiguration::New();
		AConfiguration_fromAssetManager(config->ac, appInfo.assetManager);

		diff = AConfiguration_diff(curConfig->ac, config->ac);

		curConfig = std::move(config);
	}

	//if orientation has changed
	if(diff & ACONFIGURATION_ORIENTATION){
		int32_t orientation = AConfiguration_getOrientation(curConfig->ac);
		switch(orientation){
			case ACONFIGURATION_ORIENTATION_LAND:
			case ACONFIGURATION_ORIENTATION_PORT:
				std::swap(curWinDim.x, curWinDim.y);
				break;
			case ACONFIGURATION_ORIENTATION_SQUARE:
				//do nothing
				break;
			case ACONFIGURATION_ORIENTATION_ANY:
				ASSERT(false)
			default:
				ASSERT(false)
				break;
		}
	}
}



void OnLowMemory(ANativeActivity* activity){
	TRACE(<< "OnLowMemory(): invoked" << std::endl)
	//TODO:
//    static_cast<morda::application*>(activity->instance)->OnLowMemory();
}



void OnWindowFocusChanged(ANativeActivity* activity, int hasFocus){
	TRACE(<< "OnWindowFocusChanged(): invoked" << std::endl)
}



int OnUpdateTimerExpired(int fd, int events, void* data){
//	TRACE(<< "OnUpdateTimerExpired(): invoked" << std::endl)

	auto& app = application::inst();

	std::uint32_t dt = app.gui.update();
	if(dt == 0){
		//do not arm the timer and do not clear the flag
	}else{
		fdFlag.Clear();
		timer.Arm(dt);
	}

	//after updating need to re-render everything
	render(app);

//	TRACE(<< "OnUpdateTimerExpired(): armed timer for " << dt << std::endl)

	return 1; //1 means do not remove descriptor from looper
}



int OnQueueHasMessages(int fd, int events, void* data){
	auto& ww = getImpl(getWindowPimpl(application::inst()));

	while(auto m = ww.uiQueue.peekMsg()){
		m();
	}

	return 1; //1 means do not remove descriptor from looper
}



void OnNativeWindowCreated(ANativeActivity* activity, ANativeWindow* window){
	TRACE(<< "OnNativeWindowCreated(): invoked" << std::endl)

	//save window in a static var, so it is accessible for OpenGL initializers from morda::application class
	androidWindow = window;

	curWinDim.x = float(ANativeWindow_getWidth(window));
	curWinDim.y = float(ANativeWindow_getHeight(window));

	ASSERT(!activity->instance)
	try{
		//use local auto-pointer for now because an exception can be thrown and need to delete object then.
		std::unique_ptr<AndroidConfiguration> cfg = AndroidConfiguration::New();
		//retrieve current configuration
		AConfiguration_fromAssetManager(cfg->ac, appInfo.assetManager);

		application* app = mordavokne::create_application(0, nullptr).release();

		activity->instance = app;

		//save current configuration in global variable
		curConfig = std::move(cfg);

		ALooper* looper = ALooper_prepare(0);
		ASSERT(looper)

		//Add timer descriptor to looper, this is needed for Updatable to work
		if(ALooper_addFd(
				looper,
				fdFlag.GetFD(),
				ALOOPER_POLL_CALLBACK,
				ALOOPER_EVENT_INPUT,
				&OnUpdateTimerExpired,
				0
			) == -1)
		{
			throw utki::Exc("failed to add timer descriptor to looper");
		}

		//Add UI message queue descriptor to looper
		if(ALooper_addFd(
				looper,
				static_cast<pogodi::Waitable&>(getImpl(getWindowPimpl(*app)).uiQueue).getHandle(),
				ALOOPER_POLL_CALLBACK,
				ALOOPER_EVENT_INPUT,
				&OnQueueHasMessages,
				0
			) == -1)
		{
			throw utki::Exc("failed to add UI message queue descriptor to looper");
		}

		fdFlag.Set();//this is to call the Update() for the first time if there were any Updateables started during creating application object
	}catch(std::exception& e){
		TRACE(<< "std::exception uncaught while creating application instance: " << e.what() << std::endl)
		throw;
	}catch(...){
		TRACE(<< "unknown exception uncaught while creating application instance!" << std::endl)
		throw;
	}
}



void OnNativeWindowResized(ANativeActivity* activity, ANativeWindow* window){
	TRACE(<< "OnNativeWindowResized(): invoked" << std::endl)

	//save window dimensions
	curWinDim.x = float(ANativeWindow_getWidth(window));
	curWinDim.y = float(ANativeWindow_getHeight(window));

	TRACE(<< "OnNativeWindowResized(): curWinDim = " << curWinDim << std::endl)
}





void OnNativeWindowRedrawNeeded(ANativeActivity* activity, ANativeWindow* window){
	TRACE(<< "OnNativeWindowRedrawNeeded(): invoked" << std::endl)

	render(getApp(activity));
}



//This function is called right before destroying Window object, according to documentation:
//https://developer.android.com/ndk/reference/struct/a-native-activity-callbacks#onnativewindowdestroyed
void OnNativeWindowDestroyed(ANativeActivity* activity, ANativeWindow* window){
	TRACE(<< "OnNativeWindowDestroyed(): invoked" << std::endl)

	ALooper* looper = ALooper_prepare(0);
	ASSERT(looper)

	//remove UI message queue descriptor from looper
	ALooper_removeFd(
			looper,
			static_cast<pogodi::Waitable&>(getImpl(getWindowPimpl(application::inst())).uiQueue).getHandle()
		);

	//remove fdFlag from looper
	ALooper_removeFd(looper, fdFlag.GetFD());

	//Need to destroy app right before window is destroyed, i.e. before OGL is de-initialized
	delete static_cast<mordavokne::application*>(activity->instance);
	activity->instance = nullptr;

	//delete configuration object
	curConfig.reset();
}



int OnInputEventsReadyForReadingFromQueue(int fd, int events, void* data){
//	TRACE(<< "OnInputEventsReadyForReadingFromQueue(): invoked" << std::endl)

	ASSERT(curInputQueue) //if we get events we should have input queue

	//If window is not created yet, ignore events.
	if(!mordavokne::application::isCreated()){
		ASSERT(false)
		AInputEvent* event;
		while(AInputQueue_getEvent(curInputQueue, &event) >= 0){
			if(AInputQueue_preDispatchEvent(curInputQueue, event)){
				continue;
			}

			AInputQueue_finishEvent(curInputQueue, event, false);
		}
		return 1;
	}

	ASSERT(mordavokne::application::isCreated())

	handleInputEvents();

	return 1; //we don't want to remove input queue descriptor from looper
}



void OnInputQueueCreated(ANativeActivity* activity, AInputQueue* queue){
	TRACE(<< "OnInputQueueCreated(): invoked" << std::endl)
	ASSERT(queue);
	ASSERT(!curInputQueue)
	curInputQueue = queue;

	//attach queue to looper
	AInputQueue_attachLooper(
			curInputQueue,
			ALooper_prepare(0), //get looper for current thread (main thread)
			0, //'ident' is ignored since we are using callback
			&OnInputEventsReadyForReadingFromQueue,
			activity->instance
		);
}



void OnInputQueueDestroyed(ANativeActivity* activity, AInputQueue* queue){
	TRACE(<< "OnInputQueueDestroyed(): invoked" << std::endl)
	ASSERT(queue)
	ASSERT(curInputQueue == queue)

	//detach queue from looper
	AInputQueue_detachLooper(queue);

	curInputQueue = 0;
}



//called when, for example, on-screen keyboard has been shown
void OnContentRectChanged(ANativeActivity* activity, const ARect* rect){
	TRACE(<< "OnContentRectChanged(): invoked, left = " << rect->left << " right = " << rect->right << " top = " << rect->top << " bottom = " << rect->bottom << std::endl)
	TRACE(<< "OnContentRectChanged(): curWinDim = " << curWinDim << std::endl)

	//Sometimes Android calls OnContentRectChanged() even after native window was destroyed,
	//i.e. OnNativeWindowDestroyed() was called and, thus, application object was destroyed.
	//So need to check if our application is still alive.
	if(!activity->instance){
		TRACE(<< "OnContentRectChanged(): application is not alive, ignoring content rect change." << std::endl)
		return;
	}

	auto& app = getApp(activity);

	updateWindowRect(
			app,
			morda::Rectr(
					float(rect->left),
					curWinDim.y - float(rect->bottom),
					float(rect->right - rect->left),
					float(rect->bottom - rect->top)
				)
		);

	//redraw, since WindowRedrawNeeded not always comes
	render(app);
}
}



void ANativeActivity_onCreate(
		ANativeActivity* activity,
		void* savedState,
		size_t savedStateSize
	)
{
	TRACE(<< "ANativeActivity_onCreate(): invoked" << std::endl)
	activity->callbacks->onDestroy = &OnDestroy;
	activity->callbacks->onStart = &OnStart;
	activity->callbacks->onResume = &OnResume;
	activity->callbacks->onSaveInstanceState = &OnSaveInstanceState;
	activity->callbacks->onPause = &OnPause;
	activity->callbacks->onStop = &OnStop;
	activity->callbacks->onConfigurationChanged = &OnConfigurationChanged;
	activity->callbacks->onLowMemory = &OnLowMemory;
	activity->callbacks->onWindowFocusChanged = &OnWindowFocusChanged;
	activity->callbacks->onNativeWindowCreated = &OnNativeWindowCreated;
	activity->callbacks->onNativeWindowResized = &OnNativeWindowResized;
	activity->callbacks->onNativeWindowRedrawNeeded = &OnNativeWindowRedrawNeeded;
	activity->callbacks->onNativeWindowDestroyed = &OnNativeWindowDestroyed;
	activity->callbacks->onInputQueueCreated = &OnInputQueueCreated;
	activity->callbacks->onInputQueueDestroyed = &OnInputQueueDestroyed;
	activity->callbacks->onContentRectChanged = &OnContentRectChanged;

	activity->instance = 0;

	nativeActivity = activity;

	appInfo.internalDataPath = activity->internalDataPath;
	appInfo.externalDataPath = activity->externalDataPath;
	appInfo.assetManager = activity->assetManager;

	javaFunctionsWrapper = utki::makeUnique<JavaFunctionsWrapper>(activity);
}
