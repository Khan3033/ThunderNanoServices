#include "Module.h"

MODULE_NAME_DECLARATION(BUILD_REFERENCE)

#include <virtualinput/virtualinput.h>

#include <bcm_host.h>
#include <EGL/egl.h>
#include <EGL/eglext.h>

#include <interfaces/IComposition.h>
#include "../../Client/Client.h"

int g_pipefd[2];
struct Message {
    uint32_t code;
    actiontype type;
};

static const char * connectorName = "/tmp/keyhandler";
static void VirtualKeyboardCallback(actiontype type , unsigned int code) {
    if (type != COMPLETED) {
        Message message;
        message.code = code;
        message.type = type;
        write(g_pipefd[1], &message, sizeof(message));
    }
}

namespace {

    class BCMHostInit {
        public:    

        BCMHostInit(const BCMHostInit&) = delete;
        BCMHostInit& operator=(const BCMHostInit&) = delete;

        BCMHostInit() {
            bcm_host_init();
        }

        ~BCMHostInit() {
            bcm_host_deinit();
        }

    };
}

namespace WPEFramework {

static Core::NodeId Connector () {

    string connector;
    if ((Core::SystemInfo::GetEnvironment(_T("COMPOSITOR"), connector) == false) || (connector.empty() == true)) {
        connector = _T("/tmp/compositor");
    }
    return (Core::NodeId(connector.c_str()));
}

class Display : public Compositor::IDisplay {
private:
    Display() = delete;
    Display(const Display&) = delete;
    Display& operator=(const Display&) = delete;

    class EXTERNAL CompositorClient {
    private:
        // -------------------------------------------------------------------
        // This object should not be copied or assigned. Prevent the copy
        // constructor and assignment constructor from being used. Compiler
        // generated assignment and copy methods will be blocked by the
        // following statments.
        // Define them but do not implement them, compile error/link error.
        // -------------------------------------------------------------------
        CompositorClient(const CompositorClient& a_Copy) = delete;
        CompositorClient& operator=(const CompositorClient& a_RHS) = delete;

    public:
        CompositorClient(const TCHAR formatter[], ...)
        {
            va_list ap;
            va_start(ap, formatter);
            Trace::Format(_text, formatter, ap);
            va_end(ap);
        }
        CompositorClient(const string& text)
            : _text(Core::ToString(text))
        {
        }
        ~CompositorClient() = default;

    public:
        inline const char* Data() const
        {
            return (_text.c_str());
        }
        inline uint16_t Length() const
        {
            return (static_cast<uint16_t>(_text.length()));
        }

    private:
        string _text;
    };

    class SurfaceImplementation : public Exchange::IComposition::IClient
                                , public Compositor::IDisplay::ISurface  {
    public:
        SurfaceImplementation() = delete;
        SurfaceImplementation(const SurfaceImplementation&) = delete;
        SurfaceImplementation& operator=(const SurfaceImplementation&) = delete;

        SurfaceImplementation(
                Display* compositor, const std::string& name,
                const uint32_t width, const uint32_t height);
        virtual ~SurfaceImplementation();

        using Exchange::IComposition::IClient::AddRef;

        void Opacity(const uint32_t value) override;
        void ChangedGeometry(const Exchange::IComposition::Rectangle& rectangle) override;
        void ChangedZOrder(const uint8_t zorder) override;

        virtual string Name() const override {
            return _name;
        }
        virtual void Kill() override {
            //todo: implement
            TRACE(CompositorClient, (_T("Kill called for Client %s. Not supported."), Name().c_str()));
        }
        inline EGLNativeWindowType Native() const {
            return (static_cast<EGLNativeWindowType>(_nativeSurface));
        }
        inline int32_t Width() const {
            return _width;
        }
        inline int32_t Height() const {
            return _height;
        }
        inline void Keyboard(
                Compositor::IDisplay::IKeyboard* keyboard) {
            assert((_keyboard == nullptr) ^ (keyboard == nullptr));
            _keyboard = keyboard;
        }
        inline void SendKey(
                const uint32_t key,
                const IKeyboard::state action, const uint32_t time) {
            if (_keyboard != nullptr) {
                _keyboard->Direct(key, action);
            }
        }
    
    private:

        BEGIN_INTERFACE_MAP(Entry)
            INTERFACE_ENTRY(Exchange::IComposition::IClient)
        END_INTERFACE_MAP

    private:
        Display& _display;
        const std::string _name;
        const uint32_t _width;
        const uint32_t _height;
        uint32_t _opacity;        

        EGLSurface _nativeSurface;
        EGL_DISPMANX_WINDOW_T _nativeWindow;
        DISPMANX_DISPLAY_HANDLE_T _dispmanDisplay;
        DISPMANX_UPDATE_HANDLE_T _dispmanUpdate;
        DISPMANX_ELEMENT_HANDLE_T _dispmanElement;

        VC_RECT_T _dstRect;
        VC_RECT_T _srcRect;

        IKeyboard* _keyboard;

    };

public:
    Display(const std::string& displayName);
    virtual ~Display();

    virtual void AddRef() const {
    }
    virtual uint32_t Release() const {
        return (0);
    }
    virtual EGLNativeDisplayType Native() const override {
        return (static_cast<EGLNativeDisplayType>(EGL_DEFAULT_DISPLAY));
    }
    virtual const std::string& Name() const final {
        return (_displayName);
    }
    virtual int Process (const uint32_t data) override;
    virtual int FileDescriptor() const override;
    virtual ISurface* Create(
            const std::string& name,
            const uint32_t width, const uint32_t height) override;

    inline uint32_t DisplaySizeWidth() const {
        return _displaysize.first;
    }

     inline uint32_t DisplaySizeHeight() const {
        return _displaysize.second;
    }

private:
    inline void Register(SurfaceImplementation* surface);
    inline void Unregister(SurfaceImplementation* surface);
    inline void OfferClientInterface(Exchange::IComposition::IClient* client);
    inline void RevokeClientInterface(Exchange::IComposition::IClient* client);

    using DisplaySize = std::pair<uint32_t,uint32_t>;

    inline static DisplaySize RetrieveDisplaySize() {
        DisplaySize displaysize;
        graphics_get_display_size(0, &displaysize.first, &displaysize.second);
        return displaysize;
    }

    const std::string _displayName;
    mutable Core::CriticalSection _adminLock;
    void* _virtualkeyboard;
    const DisplaySize _displaysize;
    std::list<SurfaceImplementation*> _surfaces;
    Core::ProxyType<RPC::CommunicatorClient> _compositerServerRPCConnection;
};

Display::SurfaceImplementation::SurfaceImplementation(
        Display* display,
        const std::string& name,
        const uint32_t width, const uint32_t height)
: Exchange::IComposition::IClient()
, _display(*display)
, _name(name)
, _width(width)
, _height(height)
, _opacity(255) 
, _keyboard(nullptr) {

    TRACE(CompositorClient, (_T("Created client named: %s"), _name.c_str()));

    VC_DISPMANX_ALPHA_T alpha = {
            static_cast<DISPMANX_FLAGS_ALPHA_T>
            (DISPMANX_FLAGS_ALPHA_FROM_SOURCE | DISPMANX_FLAGS_ALPHA_MIX),
            255,
            0
    };
    vc_dispmanx_rect_set(&_dstRect, 0, 0, _display.DisplaySizeWidth(), _display.DisplaySizeHeight()); 
    vc_dispmanx_rect_set(&_srcRect,
            0, 0, (_display.DisplaySizeWidth() << 16), (_display.DisplaySizeHeight() << 16)); 

    _dispmanDisplay = vc_dispmanx_display_open(0);
    _dispmanUpdate = vc_dispmanx_update_start(0);
    _dispmanElement = vc_dispmanx_element_add(
            _dispmanUpdate,
            _dispmanDisplay,
            0,
            &_dstRect,
            0 /*src*/,
            &_srcRect,
            DISPMANX_PROTECTION_NONE,
            &alpha /*alpha*/,
            0 /*clamp*/,
            DISPMANX_NO_ROTATE);
    vc_dispmanx_update_submit_sync(_dispmanUpdate);

    _nativeWindow.element = _dispmanElement;
    _nativeWindow.width = _display.DisplaySizeWidth();
    _nativeWindow.height = _display.DisplaySizeHeight();
    _nativeSurface = static_cast<EGLSurface>(&_nativeWindow);

    _display.Register(this);
}

Display::SurfaceImplementation::~SurfaceImplementation() {

    TRACE(CompositorClient, (_T("Destructing client named: %s"), _name.c_str()));

    _dispmanUpdate = vc_dispmanx_update_start(0);
    vc_dispmanx_element_remove(_dispmanUpdate, _dispmanElement);
    vc_dispmanx_update_submit_sync(_dispmanUpdate);
    vc_dispmanx_display_close(_dispmanDisplay);

    _display.Unregister(this);
}

void Display::SurfaceImplementation::Opacity(
        const uint32_t value) {

    _opacity = (value > Exchange::IComposition::maxOpacity) ? Exchange::IComposition::maxOpacity : value;

    _dispmanUpdate = vc_dispmanx_update_start(0);
    vc_dispmanx_element_change_attributes(_dispmanUpdate,
            _dispmanElement,
            (1 << 1),
            0,
            _opacity,
            &_dstRect,
            &_srcRect,
            0,
            DISPMANX_NO_ROTATE);
    vc_dispmanx_update_submit_sync(_dispmanUpdate);
}

void Display::SurfaceImplementation::ChangedGeometry(const Exchange::IComposition::Rectangle& rectangle) {
    vc_dispmanx_rect_set(&_dstRect, rectangle.x, rectangle.y, rectangle.width, rectangle.height);
    vc_dispmanx_rect_set(&_srcRect,
            0, 0, (_display.DisplaySizeWidth() << 16), (_display.DisplaySizeHeight() << 16));

    _dispmanUpdate = vc_dispmanx_update_start(0);
    vc_dispmanx_element_change_attributes(_dispmanUpdate,
            _dispmanElement,
            (1 << 2),
            0,
            _opacity,
            &_dstRect,
            &_srcRect,
            0,
            DISPMANX_NO_ROTATE);
    vc_dispmanx_update_submit_sync(_dispmanUpdate);

}
void Display::SurfaceImplementation::ChangedZOrder(const uint8_t zorder) {
   _dispmanUpdate = vc_dispmanx_update_start(0);
    vc_dispmanx_element_change_layer(_dispmanUpdate, _dispmanElement, zorder);
    vc_dispmanx_update_submit_sync(_dispmanUpdate);    
}

Display::Display(const string& name)
: _displayName(name)
, _adminLock()
, _virtualkeyboard(nullptr)
, _displaysize(RetrieveDisplaySize())
, _compositerServerRPCConnection(Core::ProxyType< RPC::CommunicatorClient >::Create(Connector(), Core::ProxyType< RPC::InvokeServerType<2,1> >::Create() ) )  {

    uint32_t result = _compositerServerRPCConnection->Open(RPC::CommunicationTimeOut);
    if ( result != Core::ERROR_NONE ) { 
        TRACE(CompositorClient, (_T("Could not open connection to Compositor with node %s. Error: %s"), _compositerServerRPCConnection->Source().RemoteId(), Core::NumberType<uint32_t>(result).Text()));
        _compositerServerRPCConnection.Release();
    }

    if (pipe(g_pipefd) == -1) {
        g_pipefd[0] = -1;
        g_pipefd[1] = -1;
    }
    _virtualkeyboard = Construct(
            name.c_str(), connectorName, VirtualKeyboardCallback);
    if (_virtualkeyboard == nullptr) {
        TRACE(CompositorClient, (_T("Initialization of virtual keyboard failed for Display %s!"), Name()));
    }
}

Display::~Display() {

    for_each(_surfaces.begin(), _surfaces.end(), [&](SurfaceImplementation* surface) {

                        string name = surface->Name();

                        if( static_cast<Core::IUnknown*>(surface)->Release() != Core::ERROR_DESTRUCTION_SUCCEEDED ) { //note, need cast to prevent ambigious call
                            TRACE(CompositorClient, (_T("Compositor Surface [%s] is not properly destructed"), name.c_str()));
                        }
                    }
            );

    if (_virtualkeyboard != nullptr) {
        Destruct(_virtualkeyboard);
    }

    if( _compositerServerRPCConnection.IsValid() == true ) {
        _compositerServerRPCConnection.Release();
    }

  }

int Display::Process(const uint32_t data) {
    Message message;
    if ((data != 0) && (g_pipefd[0] != -1) &&
            (read(g_pipefd[0], &message, sizeof(message)) > 0)) {

        _adminLock.Lock();
        std::list<SurfaceImplementation*>::iterator index(_surfaces.begin());
        while (index != _surfaces.end()) {
            // RELEASED  = 0,
            // PRESSED   = 1,
            // REPEAT    = 2,

            (*index)->SendKey (message.code, (message.type == 0 ? IDisplay::IKeyboard::released : IDisplay::IKeyboard::pressed), time(nullptr));
            index++;
        }
        _adminLock.Unlock();
    }
    return (0);
}

int Display::FileDescriptor() const {
    return (g_pipefd[0]);
}

Compositor::IDisplay::ISurface* Display::Create(
        const std::string& name, const uint32_t width, const uint32_t height) {

    SurfaceImplementation* retval = (Core::Service<SurfaceImplementation>::Create<SurfaceImplementation>(this, name, width, height));

    OfferClientInterface(retval);

   return retval;
   }

inline void Display::Register(Display::SurfaceImplementation* surface) {
    ASSERT (surface != nullptr);

    _adminLock.Lock();

    std::list<SurfaceImplementation*>::iterator index(
            std::find(_surfaces.begin(), _surfaces.end(), surface));
    if (index == _surfaces.end()) {
        _surfaces.push_back(surface);
    }

    _adminLock.Unlock();
}

inline void Display::Unregister(Display::SurfaceImplementation* surface) {
        
    ASSERT (surface != nullptr);

    _adminLock.Lock();

    std::list<SurfaceImplementation*>::iterator index(
            std::find(_surfaces.begin(), _surfaces.end(), surface));
    if (index != _surfaces.end()) {
        _surfaces.erase(index);
    }

    _adminLock.Unlock();

    RevokeClientInterface(surface);

}  

void Display::OfferClientInterface(Exchange::IComposition::IClient* client) {
    ASSERT( client != nullptr );
    uint32_t result = _compositerServerRPCConnection->Offer(client);
    if ( result != Core::ERROR_NONE ) { 
        TRACE(CompositorClient, (_T("Could not offer IClient interface with callsign %s to Compositor. Error: %s"), client->Name(), Core::NumberType<uint32_t>(result).Text()));
    }
}

void Display::RevokeClientInterface(Exchange::IComposition::IClient* client) {
    ASSERT( client != nullptr );

    uint32_t result = _compositerServerRPCConnection->Revoke(client);

    if ( result != Core::ERROR_NONE ) { 
        TRACE(CompositorClient, (_T("Could not revoke IClient interface with callsign %s to Compositor. Error: %s"), client->Name(), Core::NumberType<uint32_t>(result).Text()));
    }
}

Compositor::IDisplay* Compositor::IDisplay::Instance(const string& displayName) {
    static BCMHostInit bcmhostinit; // must be done before Display constructor
    static Display& myDisplay = Core::SingletonType<Display>::Instance(displayName);
    
    return (&myDisplay);
}

}
