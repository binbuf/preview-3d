#include "Accessibility.h"

#include <atomic>
#include <comutil.h>

#pragma comment(lib, "oleacc.lib")

namespace viewer_accessibility
{
namespace
{
bool IsSelf(const VARIANT& child) { return child.vt == VT_I4 && child.lVal == CHILDID_SELF; }

class Provider final : public IAccessible
{
public:
    Provider(HWND window, Query query, Action action, Focus focus, Status status)
        : window_(window), query_(std::move(query)), action_(std::move(action)), focus_(std::move(focus)), status_(std::move(status)) {}

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** result) override
    {
        if (!result) return E_POINTER;
        *result = nullptr;
        if (iid == IID_IUnknown || iid == IID_IDispatch || iid == IID_IAccessible) *result = static_cast<IAccessible*>(this);
        if (!*result) return E_NOINTERFACE;
        AddRef(); return S_OK;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return ++references_; }
    ULONG STDMETHODCALLTYPE Release() override { const ULONG value = --references_; if (!value) delete this; return value; }
    HRESULT STDMETHODCALLTYPE GetTypeInfoCount(UINT* count) override { if (!count) return E_POINTER; *count = 0; return S_OK; }
    HRESULT STDMETHODCALLTYPE GetTypeInfo(UINT, LCID, ITypeInfo**) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE GetIDsOfNames(REFIID, LPOLESTR*, UINT, LCID, DISPID*) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE Invoke(DISPID, REFIID, LCID, WORD, DISPPARAMS*, VARIANT*, EXCEPINFO*, UINT*) override { return E_NOTIMPL; }

    HRESULT STDMETHODCALLTYPE get_accParent(IDispatch** parent) override
    {
        if (!parent) return E_POINTER; *parent = nullptr;
        return AccessibleObjectFromWindow(GetParent(window_), OBJID_WINDOW, IID_IDispatch, reinterpret_cast<void**>(parent));
    }
    HRESULT STDMETHODCALLTYPE get_accChildCount(long* count) override
    {
        if (!count) return E_POINTER; *count = static_cast<long>(Visible().size()); return S_OK;
    }
    HRESULT STDMETHODCALLTYPE get_accChild(VARIANT, IDispatch** child) override { if (!child) return E_POINTER; *child = nullptr; return S_FALSE; }
    HRESULT STDMETHODCALLTYPE get_accName(VARIANT child, BSTR* name) override
    {
        if (!name) return E_POINTER; *name = nullptr;
        const std::wstring value = IsSelf(child) ? L"Preview 3D" : Info(child).name;
        if (value.empty()) return S_FALSE; *name = SysAllocString(value.c_str()); return *name ? S_OK : E_OUTOFMEMORY;
    }
    HRESULT STDMETHODCALLTYPE get_accValue(VARIANT child, BSTR* value) override
    {
        if (!value) return E_POINTER; *value = nullptr;
        const std::wstring text = IsSelf(child) ? status_() : Info(child).value;
        if (text.empty()) return S_FALSE; *value = SysAllocString(text.c_str()); return *value ? S_OK : E_OUTOFMEMORY;
    }
    HRESULT STDMETHODCALLTYPE get_accDescription(VARIANT child, BSTR* description) override
    {
        if (!description) return E_POINTER; *description = nullptr;
        const std::wstring text = IsSelf(child) ? status_() : Info(child).description;
        if (text.empty()) return S_FALSE; *description = SysAllocString(text.c_str()); return *description ? S_OK : E_OUTOFMEMORY;
    }
    HRESULT STDMETHODCALLTYPE get_accRole(VARIANT child, VARIANT* role) override
    {
        if (!role) return E_POINTER; VariantInit(role); role->vt = VT_I4;
        role->lVal = IsSelf(child) ? ROLE_SYSTEM_CLIENT : Info(child).role; return S_OK;
    }
    HRESULT STDMETHODCALLTYPE get_accState(VARIANT child, VARIANT* state) override
    {
        if (!state) return E_POINTER; VariantInit(state); state->vt = VT_I4;
        if (IsSelf(child)) { state->lVal = STATE_SYSTEM_FOCUSABLE; return S_OK; }
        const ControlInfo info = Info(child);
        state->lVal = STATE_SYSTEM_FOCUSABLE;
        if (!info.visible) state->lVal |= STATE_SYSTEM_INVISIBLE;
        if (!info.enabled) state->lVal |= STATE_SYSTEM_UNAVAILABLE;
        if (info.checked) state->lVal |= STATE_SYSTEM_CHECKED;
        if (info.focused) state->lVal |= STATE_SYSTEM_FOCUSED;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE get_accHelp(VARIANT child, BSTR* help) override { return get_accDescription(child, help); }
    HRESULT STDMETHODCALLTYPE get_accHelpTopic(BSTR*, VARIANT, long*) override { return S_FALSE; }
    HRESULT STDMETHODCALLTYPE get_accKeyboardShortcut(VARIANT, BSTR*) override { return S_FALSE; }
    HRESULT STDMETHODCALLTYPE get_accFocus(VARIANT* focus) override
    {
        if (!focus) return E_POINTER; VariantInit(focus);
        const auto controls = Visible();
        for (std::size_t i = 0; i < controls.size(); ++i) if (query_(controls[i]).focused) {
            focus->vt = VT_I4; focus->lVal = static_cast<LONG>(i + 1); return S_OK;
        }
        focus->vt = VT_EMPTY; return S_OK;
    }
    HRESULT STDMETHODCALLTYPE get_accSelection(VARIANT* selection) override { if (!selection) return E_POINTER; VariantInit(selection); return S_OK; }
    HRESULT STDMETHODCALLTYPE get_accDefaultAction(VARIANT child, BSTR* action) override
    {
        if (!action) return E_POINTER; *action = nullptr;
        if (IsSelf(child)) return S_FALSE;
        const LONG role = Info(child).role;
        const wchar_t* text = role == ROLE_SYSTEM_SLIDER ? L"Adjust" : role == ROLE_SYSTEM_CHECKBUTTON ? L"Toggle" : L"Press";
        *action = SysAllocString(text); return *action ? S_OK : E_OUTOFMEMORY;
    }
    HRESULT STDMETHODCALLTYPE accSelect(long flags, VARIANT child) override
    {
        if (IsSelf(child)) { if (flags & SELFLAG_TAKEFOCUS) focus_(Control::None); return S_OK; }
        const Control control = Child(child);
        if (control == Control::None) return E_INVALIDARG;
        if (flags & SELFLAG_TAKEFOCUS) { focus_(control); NotifyWinEvent(EVENT_OBJECT_FOCUS, window_, OBJID_CLIENT, child.lVal); }
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE accLocation(long* left, long* top, long* width, long* height, VARIANT child) override
    {
        if (!left || !top || !width || !height) return E_POINTER;
        RECT rect{};
        if (IsSelf(child)) GetClientRect(window_, &rect); else rect = Info(child).rect;
        POINT point{rect.left, rect.top}; ClientToScreen(window_, &point);
        *left = point.x; *top = point.y; *width = rect.right - rect.left; *height = rect.bottom - rect.top; return S_OK;
    }
    HRESULT STDMETHODCALLTYPE accNavigate(long direction, VARIANT from, VARIANT* target) override
    {
        if (!target) return E_POINTER; VariantInit(target);
        const auto controls = Visible();
        if (controls.empty()) return S_FALSE;
        long index = IsSelf(from) ? -1 : from.lVal - 1;
        if (direction == NAVDIR_FIRSTCHILD) index = 0;
        else if (direction == NAVDIR_LASTCHILD) index = static_cast<long>(controls.size() - 1);
        else if (direction == NAVDIR_NEXT) ++index;
        else if (direction == NAVDIR_PREVIOUS) --index;
        else return S_FALSE;
        if (index < 0 || index >= static_cast<long>(controls.size())) return S_FALSE;
        target->vt = VT_I4; target->lVal = index + 1; return S_OK;
    }
    HRESULT STDMETHODCALLTYPE accHitTest(long x, long y, VARIANT* child) override
    {
        if (!child) return E_POINTER; VariantInit(child);
        POINT point{x, y}; ScreenToClient(window_, &point);
        const auto controls = Visible();
        for (std::size_t i = 0; i < controls.size(); ++i) {
            const RECT rect = query_(controls[i]).rect;
            if (PtInRect(&rect, point)) { child->vt = VT_I4; child->lVal = static_cast<LONG>(i + 1); return S_OK; }
        }
        child->vt = VT_I4; child->lVal = CHILDID_SELF; return S_OK;
    }
    HRESULT STDMETHODCALLTYPE accDoDefaultAction(VARIANT child) override
    {
        const Control control = Child(child); if (control == Control::None) return E_INVALIDARG;
        action_(control); return S_OK;
    }
    HRESULT STDMETHODCALLTYPE put_accName(VARIANT, BSTR) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE put_accValue(VARIANT, BSTR) override { return E_NOTIMPL; }

private:
    std::vector<Control> Visible() const { return VisibleControls(query_); }
    Control Child(const VARIANT& child) const
    {
        if (child.vt != VT_I4 || child.lVal <= 0) return Control::None;
        const auto controls = Visible(); const std::size_t index = static_cast<std::size_t>(child.lVal - 1);
        return index < controls.size() ? controls[index] : Control::None;
    }
    ControlInfo Info(const VARIANT& child) const
    {
        const Control control = Child(child); return control == Control::None ? ControlInfo{} : query_(control);
    }

    std::atomic<ULONG> references_{1};
    HWND window_ = nullptr;
    Query query_;
    Action action_;
    Focus focus_;
    Status status_;
};

class UiaRoot;

class UiaChild final : public IRawElementProviderSimple, public IRawElementProviderFragment,
    public IInvokeProvider, public IToggleProvider, public IValueProvider
{
public:
    UiaChild(UiaRoot* root, Control control);
    ~UiaChild();
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** result) override;
    ULONG STDMETHODCALLTYPE AddRef() override { return ++references_; }
    ULONG STDMETHODCALLTYPE Release() override { const ULONG value=--references_; if(!value) delete this; return value; }
    HRESULT STDMETHODCALLTYPE get_ProviderOptions(ProviderOptions* options) override;
    HRESULT STDMETHODCALLTYPE GetPatternProvider(PATTERNID id, IUnknown** provider) override;
    HRESULT STDMETHODCALLTYPE GetPropertyValue(PROPERTYID id, VARIANT* value) override;
    HRESULT STDMETHODCALLTYPE get_HostRawElementProvider(IRawElementProviderSimple** provider) override;
    HRESULT STDMETHODCALLTYPE Navigate(NavigateDirection direction, IRawElementProviderFragment** result) override;
    HRESULT STDMETHODCALLTYPE GetRuntimeId(SAFEARRAY** runtimeId) override;
    HRESULT STDMETHODCALLTYPE get_BoundingRectangle(UiaRect* rect) override;
    HRESULT STDMETHODCALLTYPE GetEmbeddedFragmentRoots(SAFEARRAY** roots) override;
    HRESULT STDMETHODCALLTYPE SetFocus() override;
    HRESULT STDMETHODCALLTYPE get_FragmentRoot(IRawElementProviderFragmentRoot** root) override;
    HRESULT STDMETHODCALLTYPE Invoke() override;
    HRESULT STDMETHODCALLTYPE Toggle() override;
    HRESULT STDMETHODCALLTYPE get_ToggleState(ToggleState* state) override;
    HRESULT STDMETHODCALLTYPE SetValue(LPCWSTR) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE get_Value(BSTR* value) override;
    HRESULT STDMETHODCALLTYPE get_IsReadOnly(BOOL* value) override;
private:
    std::atomic<ULONG> references_{1}; UiaRoot* root_; Control control_;
};

class UiaRoot final : public IRawElementProviderSimple, public IRawElementProviderFragment,
    public IRawElementProviderFragmentRoot
{
public:
    UiaRoot(HWND window, Query query, Action action, Focus focus, Status status)
        : window(window), query(std::move(query)), action(std::move(action)), focus(std::move(focus)), status(std::move(status)) {}
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid,void** result) override
    {
        if(!result) return E_POINTER; *result=nullptr;
        if(iid==IID_IUnknown || iid==__uuidof(IRawElementProviderSimple)) *result=static_cast<IRawElementProviderSimple*>(this);
        else if(iid==__uuidof(IRawElementProviderFragment)) *result=static_cast<IRawElementProviderFragment*>(this);
        else if(iid==__uuidof(IRawElementProviderFragmentRoot)) *result=static_cast<IRawElementProviderFragmentRoot*>(this);
        if(!*result) return E_NOINTERFACE; AddRef(); return S_OK;
    }
    ULONG STDMETHODCALLTYPE AddRef() override{return ++references;}
    ULONG STDMETHODCALLTYPE Release() override{const ULONG value=--references;if(!value)delete this;return value;}
    HRESULT STDMETHODCALLTYPE get_ProviderOptions(ProviderOptions* options) override{if(!options)return E_POINTER;*options=ProviderOptions_ServerSideProvider;return S_OK;}
    HRESULT STDMETHODCALLTYPE GetPatternProvider(PATTERNID, IUnknown** provider) override{if(!provider)return E_POINTER;*provider=nullptr;return S_OK;}
    HRESULT STDMETHODCALLTYPE GetPropertyValue(PROPERTYID id,VARIANT* value) override
    {
        if(!value)return E_POINTER;VariantInit(value);
        if(id==UIA_NamePropertyId){value->vt=VT_BSTR;value->bstrVal=SysAllocString(L"Preview 3D");}
        else if(id==UIA_ControlTypePropertyId){value->vt=VT_I4;value->lVal=UIA_WindowControlTypeId;}
        else if(id==UIA_NativeWindowHandlePropertyId){value->vt=VT_I4;value->lVal=static_cast<LONG>(reinterpret_cast<LONG_PTR>(window));}
        else if(id==UIA_IsKeyboardFocusablePropertyId){value->vt=VT_BOOL;value->boolVal=VARIANT_TRUE;}
        else if(id==UIA_IsControlElementPropertyId || id==UIA_IsContentElementPropertyId){value->vt=VT_BOOL;value->boolVal=VARIANT_TRUE;}
        else if(id==UIA_HelpTextPropertyId){value->vt=VT_BSTR;value->bstrVal=SysAllocString(status().c_str());}
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE get_HostRawElementProvider(IRawElementProviderSimple** provider) override{if(!provider)return E_POINTER;*provider=nullptr;return S_OK;}
    HRESULT STDMETHODCALLTYPE Navigate(NavigateDirection direction,IRawElementProviderFragment** result) override
    {
        if(!result)return E_POINTER;*result=nullptr;const auto visible=VisibleControls(query);
        if(visible.empty())return S_OK;
        if(direction==NavigateDirection_FirstChild)return Child(visible.front(),result);
        if(direction==NavigateDirection_LastChild)return Child(visible.back(),result);
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetRuntimeId(SAFEARRAY** runtimeId) override{if(!runtimeId)return E_POINTER;*runtimeId=nullptr;return S_OK;}
    HRESULT STDMETHODCALLTYPE get_BoundingRectangle(UiaRect* rect) override
    {
        if(!rect)return E_POINTER;RECT client{};GetClientRect(window,&client);POINT origin{0,0};ClientToScreen(window,&origin);
        *rect={double(origin.x),double(origin.y),double(client.right),double(client.bottom)};return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetEmbeddedFragmentRoots(SAFEARRAY** roots) override{if(!roots)return E_POINTER;*roots=nullptr;return S_OK;}
    HRESULT STDMETHODCALLTYPE SetFocus() override{focus(Control::None);return S_OK;}
    HRESULT STDMETHODCALLTYPE get_FragmentRoot(IRawElementProviderFragmentRoot** root) override{if(!root)return E_POINTER;*root=this;AddRef();return S_OK;}
    HRESULT STDMETHODCALLTYPE ElementProviderFromPoint(double x,double y,IRawElementProviderFragment** result) override
    {
        if(!result)return E_POINTER;*result=nullptr;POINT point{LONG(x),LONG(y)};ScreenToClient(window,&point);
        for(Control control:VisibleControls(query)){const RECT rect=query(control).rect;if(PtInRect(&rect,point))return Child(control,result);}
        *result=this;AddRef();return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetFocus(IRawElementProviderFragment** result) override
    {
        if(!result)return E_POINTER;*result=nullptr;for(Control control:VisibleControls(query))if(query(control).focused)return Child(control,result);return S_OK;
    }
    HRESULT Child(Control control,IRawElementProviderFragment** result){*result=new(std::nothrow)UiaChild(this,control);return *result?S_OK:E_OUTOFMEMORY;}
    HWND window; Query query; Action action; Focus focus; Status status;
private:
    std::atomic<ULONG> references{1};
};

UiaChild::UiaChild(UiaRoot* root,Control control):root_(root),control_(control){root_->AddRef();}
UiaChild::~UiaChild(){root_->Release();}
HRESULT UiaChild::QueryInterface(REFIID iid,void** result)
{
    if(!result)return E_POINTER;*result=nullptr;const auto info=root_->query(control_);
    if(iid==IID_IUnknown || iid==__uuidof(IRawElementProviderSimple))*result=static_cast<IRawElementProviderSimple*>(this);
    else if(iid==__uuidof(IRawElementProviderFragment))*result=static_cast<IRawElementProviderFragment*>(this);
    else if(iid==__uuidof(IInvokeProvider) && info.role!=ROLE_SYSTEM_SLIDER)*result=static_cast<IInvokeProvider*>(this);
    else if(iid==__uuidof(IToggleProvider) && info.role==ROLE_SYSTEM_CHECKBUTTON)*result=static_cast<IToggleProvider*>(this);
    else if(iid==__uuidof(IValueProvider) && !info.value.empty())*result=static_cast<IValueProvider*>(this);
    if(!*result)return E_NOINTERFACE;AddRef();return S_OK;
}
HRESULT UiaChild::get_ProviderOptions(ProviderOptions* options){if(!options)return E_POINTER;*options=ProviderOptions_ServerSideProvider;return S_OK;}
HRESULT UiaChild::GetPatternProvider(PATTERNID id,IUnknown** provider)
{
    if(!provider)return E_POINTER;*provider=nullptr;const auto info=root_->query(control_);
    if(id==UIA_TogglePatternId && info.role==ROLE_SYSTEM_CHECKBUTTON)*provider=static_cast<IToggleProvider*>(this);
    else if(id==UIA_InvokePatternId && info.role!=ROLE_SYSTEM_SLIDER)*provider=static_cast<IInvokeProvider*>(this);
    else if(id==UIA_ValuePatternId && !info.value.empty())*provider=static_cast<IValueProvider*>(this);
    if(*provider)AddRef();return S_OK;
}
HRESULT UiaChild::GetPropertyValue(PROPERTYID id,VARIANT* value)
{
    if(!value)return E_POINTER;VariantInit(value);const auto info=root_->query(control_);
    if(id==UIA_NamePropertyId){value->vt=VT_BSTR;value->bstrVal=SysAllocString(info.name.c_str());}
    else if(id==UIA_HelpTextPropertyId){value->vt=VT_BSTR;value->bstrVal=SysAllocString(info.description.c_str());}
    else if(id==UIA_ValueValuePropertyId && !info.value.empty()){value->vt=VT_BSTR;value->bstrVal=SysAllocString(info.value.c_str());}
    else if(id==UIA_AutomationIdPropertyId){value->vt=VT_BSTR;const std::wstring idText=L"Preview3D.Control."+std::to_wstring(static_cast<int>(control_));value->bstrVal=SysAllocString(idText.c_str());}
    else if(id==UIA_ControlTypePropertyId){value->vt=VT_I4;value->lVal=info.role==ROLE_SYSTEM_SLIDER?UIA_SliderControlTypeId:info.role==ROLE_SYSTEM_CHECKBUTTON?UIA_CheckBoxControlTypeId:info.role==ROLE_SYSTEM_RADIOBUTTON?UIA_RadioButtonControlTypeId:UIA_ButtonControlTypeId;}
    else if(id==UIA_IsEnabledPropertyId){value->vt=VT_BOOL;value->boolVal=info.enabled?VARIANT_TRUE:VARIANT_FALSE;}
    else if(id==UIA_IsKeyboardFocusablePropertyId){value->vt=VT_BOOL;value->boolVal=VARIANT_TRUE;}
    else if(id==UIA_HasKeyboardFocusPropertyId){value->vt=VT_BOOL;value->boolVal=info.focused?VARIANT_TRUE:VARIANT_FALSE;}
    else if(id==UIA_IsOffscreenPropertyId){value->vt=VT_BOOL;value->boolVal=info.visible?VARIANT_FALSE:VARIANT_TRUE;}
    else if(id==UIA_IsControlElementPropertyId || id==UIA_IsContentElementPropertyId){value->vt=VT_BOOL;value->boolVal=VARIANT_TRUE;}
    return S_OK;
}
HRESULT UiaChild::get_HostRawElementProvider(IRawElementProviderSimple** provider){if(!provider)return E_POINTER;*provider=nullptr;return S_OK;}
HRESULT UiaChild::Navigate(NavigateDirection direction,IRawElementProviderFragment** result)
{
    if(!result)return E_POINTER;*result=nullptr;if(direction==NavigateDirection_Parent){*result=root_;root_->AddRef();return S_OK;}
    const auto visible=VisibleControls(root_->query);const auto found=std::find(visible.begin(),visible.end(),control_);if(found==visible.end())return S_OK;
    auto target=found;if(direction==NavigateDirection_NextSibling){if(++target==visible.end())return S_OK;}
    else if(direction==NavigateDirection_PreviousSibling){if(target==visible.begin())return S_OK;--target;}else return S_OK;
    return root_->Child(*target,result);
}
HRESULT UiaChild::GetRuntimeId(SAFEARRAY** runtimeId)
{
    if(!runtimeId)return E_POINTER;int values[2]={UiaAppendRuntimeId,static_cast<int>(control_)};
    SAFEARRAY* array=SafeArrayCreateVector(VT_I4,0,2);if(!array)return E_OUTOFMEMORY;
    for(LONG index=0;index<2;++index)SafeArrayPutElement(array,&index,&values[index]);*runtimeId=array;return S_OK;
}
HRESULT UiaChild::get_BoundingRectangle(UiaRect* rect)
{
    if(!rect)return E_POINTER;const RECT client=root_->query(control_).rect;POINT origin{client.left,client.top};ClientToScreen(root_->window,&origin);
    *rect={double(origin.x),double(origin.y),double(client.right-client.left),double(client.bottom-client.top)};return S_OK;
}
HRESULT UiaChild::GetEmbeddedFragmentRoots(SAFEARRAY** roots){if(!roots)return E_POINTER;*roots=nullptr;return S_OK;}
HRESULT UiaChild::SetFocus(){root_->focus(control_);return S_OK;}
HRESULT UiaChild::get_FragmentRoot(IRawElementProviderFragmentRoot** root){if(!root)return E_POINTER;*root=root_;root_->AddRef();return S_OK;}
HRESULT UiaChild::Invoke(){if(!root_->query(control_).enabled)return UIA_E_ELEMENTNOTENABLED;root_->action(control_);return S_OK;}
HRESULT UiaChild::Toggle(){return Invoke();}
HRESULT UiaChild::get_ToggleState(ToggleState* state){if(!state)return E_POINTER;*state=root_->query(control_).checked?ToggleState_On:ToggleState_Off;return S_OK;}
HRESULT UiaChild::get_Value(BSTR* value)
{
    if(!value)return E_POINTER;*value=nullptr;const auto text=root_->query(control_).value;
    if(text.empty())return S_FALSE;*value=SysAllocString(text.c_str());return *value?S_OK:E_OUTOFMEMORY;
}
HRESULT UiaChild::get_IsReadOnly(BOOL* value){if(!value)return E_POINTER;*value=TRUE;return S_OK;}
}

std::vector<Control> VisibleControls(const Query& query)
{
    std::vector<Control> result;
    static constexpr Control ordered[] = {
        Control::Grid, Control::GroundAxis, Control::GroundDirection, Control::AxisSnap, Control::Speed,
        Control::Fit, Control::Reset, Control::Share, Control::More,
        Control::OpenWith, Control::Minimize, Control::Maximize, Control::Close,
        Control::Info, Control::InfoPanelClose, Control::LightingStudio, Control::LightingClay,
        Control::LightingDirectional, Control::DirectionalLightAngle, Control::DirectionalLightElevation, Control::Wireframe,
        Control::Zoom, Control::Fullscreen, Control::SpeedSlider,
        Control::NativeOrientation, Control::HideCursorWhileDragging,
        Control::GizmoPositiveX, Control::GizmoNegativeX,
        Control::GizmoPositiveY, Control::GizmoNegativeY,
        Control::GizmoPositiveZ, Control::GizmoNegativeZ,
        Control::ErrorRetry, Control::ErrorOpenAnother, Control::ErrorCopyDetails,
    };
    for (const Control control : ordered) {
        if (query(control).visible) result.push_back(control);
    }
    return result;
}

IAccessible* CreateProvider(HWND window, Query query, Action action, Focus focus, Status status)
{
    return new (std::nothrow) Provider(window, std::move(query), std::move(action), std::move(focus), std::move(status));
}

IRawElementProviderSimple* CreateUiaProvider(HWND window, Query query, Action action, Focus focus, Status status)
{
    return new(std::nothrow)UiaRoot(window,std::move(query),std::move(action),std::move(focus),std::move(status));
}

void Announce(HWND window, IRawElementProviderSimple* provider, const std::wstring& text)
{
    NotifyWinEvent(EVENT_SYSTEM_ALERT, window, OBJID_CLIENT, CHILDID_SELF);
    NotifyWinEvent(EVENT_OBJECT_VALUECHANGE, window, OBJID_CLIENT, CHILDID_SELF);
    if(provider && !text.empty()) {
        BSTR announcement=SysAllocString(text.c_str());
        BSTR activity=SysAllocString(L"Preview3D.DocumentStatus");
        UiaRaiseNotificationEvent(provider,NotificationKind_ActionCompleted,
            NotificationProcessing_MostRecent,announcement,activity);
        SysFreeString(announcement);
        SysFreeString(activity);
    }
}
}
