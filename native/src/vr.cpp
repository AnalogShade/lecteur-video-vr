#include "vr.h"
#include "vr-ui.h"
#define XR_USE_PLATFORM_WIN32
#define XR_USE_GRAPHICS_API_D3D11
#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>
#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstring>
#include <vector>

namespace {
constexpr XrPosef identity{{0, 0, 0, 1}, {0, 0, 0}};

void xrCheck(XrInstance instance, XrResult result, const char* context) {
    if (XR_SUCCEEDED(result)) return;
    char reason[XR_MAX_RESULT_STRING_SIZE]{};
    if (!instance || XR_FAILED(xrResultToString(instance, result, reason)))
        throw std::runtime_error(std::string(context) + " (OpenXR " + std::to_string(result) + ")");
    throw std::runtime_error(std::string(context) + " (" + reason + ")");
}

XMFLOAT4 rotation(const XrQuaternionf& q) { return {q.x, q.y, q.z, q.w}; }

struct Swapchain {
    XrSwapchain handle = XR_NULL_HANDLE;
    UINT width = 0, height = 0;
    std::vector<ComPtr<ID3D11RenderTargetView>> targets;
};
}

struct Vr::State {
    XrInstance instance = XR_NULL_HANDLE;
    XrSystemId system = XR_NULL_SYSTEM_ID;
    XrSession session = XR_NULL_HANDLE;
    XrSpace local = XR_NULL_HANDLE, viewer = XR_NULL_HANDLE;
    XrGraphicsRequirementsD3D11KHR requirements{XR_TYPE_GRAPHICS_REQUIREMENTS_D3D11_KHR};
    XrActionSet actions = XR_NULL_HANDLE;
    XrAction gripPose = XR_NULL_HANDLE, aimPose = XR_NULL_HANDLE, trigger = XR_NULL_HANDLE, squeeze = XR_NULL_HANDLE, stick = XR_NULL_HANDLE;
    XrAction select = XR_NULL_HANDLE, back = XR_NULL_HANDLE, center = XR_NULL_HANDLE;
    std::array<XrPath, 2> hands{};
    std::array<XrSpace, 2> grips{}, aims{};
    std::array<bool, 2> triggerHeld{}, gripHeld{};
    VrUi::ConfirmPress confirmPress;
    VrUi::FileGesture fileGesture;
    unsigned pointingHand = 1;
    int capturedPointer = -1;
    int capturedTrigger = -1;
    std::array<Swapchain, 2> eyes{};
    Swapchain menuSurface;
    DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
    bool running = false;
    bool pendingCenter = true;
    VrUi::Surface placedSurface = VrUi::Surface::None;
    XrPosef panel = identity;
    int grabbing = -1;
    int stickStep = 0;
    std::chrono::steady_clock::time_point repeatAt{};
    XMFLOAT4 orientation{0, 0, 0, 1}, startHand{0, 0, 0, 1}, startView{0, 0, 0, 1};

    ~State() {
        close();
        if (actions) xrDestroyActionSet(actions);
        if (instance) xrDestroyInstance(instance);
    }

    void check(XrResult result, const char* operation) const { xrCheck(instance, result, operation); }

    XrPath path(const std::string& value) const {
        XrPath result{};
        check(xrStringToPath(instance, value.c_str(), &result), "Chemin de la manette");
        return result;
    }

    XrAction action(const char* name, const char* label, XrActionType type) {
        XrActionCreateInfo info{XR_TYPE_ACTION_CREATE_INFO};
        strcpy_s(info.actionName, name);
        strcpy_s(info.localizedActionName, label);
        info.actionType = type;
        info.countSubactionPaths = static_cast<uint32_t>(hands.size());
        info.subactionPaths = hands.data();
        XrAction result{};
        check(xrCreateAction(actions, &info, &result), "Création d’une commande Touch");
        return result;
    }

    void initActions() {
        XrActionSetCreateInfo info{XR_TYPE_ACTION_SET_CREATE_INFO};
        strcpy_s(info.actionSetName, "lecteur_video_vr");
        strcpy_s(info.localizedActionSetName, "Lecteur vidéo VR");
        check(xrCreateActionSet(instance, &info, &actions), "Commandes Oculus Touch");
        hands = {path("/user/hand/left"), path("/user/hand/right")};
        gripPose = action("grip_pose", "Orientation de la manette", XR_ACTION_TYPE_POSE_INPUT);
        aimPose = action("point", "Pointer l’interface", XR_ACTION_TYPE_POSE_INPUT);
        trigger = action("trigger", "Confirmer ou saisir la vue", XR_ACTION_TYPE_FLOAT_INPUT);
        squeeze = action("squeeze", "Orienter la vidéo ou changer de fichier", XR_ACTION_TYPE_FLOAT_INPUT);
        stick = action("navigate", "Naviguer", XR_ACTION_TYPE_VECTOR2F_INPUT);
        select = action("select", "Confirmer ou lire", XR_ACTION_TYPE_BOOLEAN_INPUT);
        back = action("back", "Précédent", XR_ACTION_TYPE_BOOLEAN_INPUT);
        center = action("center", "Recentrer", XR_ACTION_TYPE_BOOLEAN_INPUT);
        std::vector<XrActionSuggestedBinding> bindings;
        for (unsigned i = 0; i < hands.size(); ++i) {
            const std::string hand = i == 0 ? "/user/hand/left/input/" : "/user/hand/right/input/";
            bindings.push_back({gripPose, path(hand + "grip/pose")});
            bindings.push_back({aimPose, path(hand + "aim/pose")});
            bindings.push_back({trigger, path(hand + "trigger/value")});
            bindings.push_back({squeeze, path(hand + "squeeze/value")});
            bindings.push_back({stick, path(hand + "thumbstick")});
            bindings.push_back({back, path(hand + (i == 0 ? "y/click" : "b/click"))});
            bindings.push_back({select, path(hand + (i == 0 ? "x/click" : "a/click"))});
            bindings.push_back({center, path(hand + "thumbstick/click")});
        }
        XrInteractionProfileSuggestedBinding infoBind{XR_TYPE_INTERACTION_PROFILE_SUGGESTED_BINDING};
        infoBind.interactionProfile = path("/interaction_profiles/oculus/touch_controller");
        infoBind.countSuggestedBindings = static_cast<uint32_t>(bindings.size());
        infoBind.suggestedBindings = bindings.data();
        check(xrSuggestInteractionProfileBindings(instance, &infoBind), "Assignation des boutons Oculus Touch");
    }

    void makeSpace(XrReferenceSpaceType type, XrSpace& space) {
        XrReferenceSpaceCreateInfo info{XR_TYPE_REFERENCE_SPACE_CREATE_INFO};
        info.referenceSpaceType = type;
        info.poseInReferenceSpace = identity;
        check(xrCreateReferenceSpace(session, &info, &space), "Création de l’espace VR");
    }

    void makeSwapchain(ID3D11Device* device, Swapchain& swap, UINT width, UINT height) {
        swap.width = width;
        swap.height = height;
        XrSwapchainCreateInfo info{XR_TYPE_SWAPCHAIN_CREATE_INFO};
        info.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT;
        info.format = format;
        info.sampleCount = info.faceCount = info.arraySize = info.mipCount = 1;
        info.width = width;
        info.height = height;
        check(xrCreateSwapchain(session, &info, &swap.handle), "Création d’une surface VR");
        uint32_t size = 0;
        check(xrEnumerateSwapchainImages(swap.handle, 0, &size, nullptr), "Images VR disponibles");
        std::vector<XrSwapchainImageD3D11KHR> images(size, {XR_TYPE_SWAPCHAIN_IMAGE_D3D11_KHR});
        check(xrEnumerateSwapchainImages(swap.handle, size, &size,
              reinterpret_cast<XrSwapchainImageBaseHeader*>(images.data())), "Images VR");
        swap.targets.resize(size);
        D3D11_RENDER_TARGET_VIEW_DESC desc{};
        desc.Format = format;
        desc.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;
        for (uint32_t i = 0; i < size; ++i) {
            checked(device->CreateRenderTargetView(images[i].texture, &desc, &swap.targets[i]), "Surface Direct3D du casque");
        }
    }

    void close() {
        for (auto* swap : {&eyes[0], &eyes[1], &menuSurface}) {
            swap->targets.clear();
            if (swap->handle) xrDestroySwapchain(swap->handle);
            swap->handle = XR_NULL_HANDLE;
        }
        for (auto* space : {&local, &viewer, &grips[0], &grips[1], &aims[0], &aims[1]}) {
            if (*space) xrDestroySpace(*space);
            *space = XR_NULL_HANDLE;
        }
        if (session) xrDestroySession(session);
        session = XR_NULL_HANDLE;
        running = false;
        placedSurface = VrUi::Surface::None;
        triggerHeld.fill(false);
        gripHeld.fill(false);
        confirmPress = {};
        fileGesture = {};
        pointingHand = 1;
        capturedPointer = -1;
        capturedTrigger = -1;
        grabbing = -1;
        stickStep = 0;
    }

    void poll() {
        XrEventDataBuffer event{XR_TYPE_EVENT_DATA_BUFFER};
        for (;;) {
            XrResult found = xrPollEvent(instance, &event);
            if (found == XR_EVENT_UNAVAILABLE) return;
            check(found, "État du casque");
            if (event.type == XR_TYPE_EVENT_DATA_INSTANCE_LOSS_PENDING) { close(); return; }
            if (event.type == XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED) {
                const auto& change = reinterpret_cast<const XrEventDataSessionStateChanged&>(event);
                if (change.session == session) {
                    if (change.state == XR_SESSION_STATE_READY) {
                        XrSessionBeginInfo begin{XR_TYPE_SESSION_BEGIN_INFO};
                        begin.primaryViewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
                        check(xrBeginSession(session, &begin), "Démarrage dans le casque");
                        running = true;
                    } else if (change.state == XR_SESSION_STATE_STOPPING) {
                        check(xrEndSession(session), "Fin de la session VR");
                        running = false;
                    } else if (change.state == XR_SESSION_STATE_EXITING || change.state == XR_SESSION_STATE_LOSS_PENDING) {
                        close();
                        return;
                    }
                }
            }
            event = {XR_TYPE_EVENT_DATA_BUFFER};
        }
    }

    template<class Draw>
    void render(Swapchain& surface, Draw draw) {
        XrSwapchainImageAcquireInfo take{XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
        uint32_t index = 0;
        check(xrAcquireSwapchainImage(surface.handle, &take, &index), "Acquisition d’une image VR");
        XrSwapchainImageWaitInfo wait{XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};
        wait.timeout = XR_INFINITE_DURATION;
        check(xrWaitSwapchainImage(surface.handle, &wait), "Attente de l’image du casque");
        draw(surface.targets[index].Get());
        XrSwapchainImageReleaseInfo release{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
        check(xrReleaseSwapchainImage(surface.handle, &release), "Présentation d’une image VR");
    }

    XrSwapchainSubImage subImage(const Swapchain& swap) const {
        return {swap.handle, {{0, 0}, {static_cast<int32_t>(swap.width), static_cast<int32_t>(swap.height)}}, 0};
    }

    bool placePanel(XrTime time, VrUi::Surface surface) {
        if (placedSurface == surface) return true;
        XrSpaceLocation head{XR_TYPE_SPACE_LOCATION};
        constexpr auto poseValid = XR_SPACE_LOCATION_POSITION_VALID_BIT | XR_SPACE_LOCATION_ORIENTATION_VALID_BIT;
        if (XR_FAILED(xrLocateSpace(viewer, local, time, &head)) || (head.locationFlags & poseValid) != poseValid) return false;
        const auto q = rotation(head.pose.orientation);
        const auto forward = XMVector3Rotate(XMVectorSet(0, 0, -1, 0), XMLoadFloat4(&q));
        const float heading = std::atan2(-XMVectorGetX(forward), -XMVectorGetZ(forward));
        const auto level = XMQuaternionRotationRollPitchYaw(0, heading, 0);
        XMFLOAT4 orientation;
        XMFLOAT3 offset;
        XMStoreFloat4(&orientation, level);
        const bool timeline = surface == VrUi::Surface::Timeline;
        XMStoreFloat3(&offset, XMVector3Rotate(XMVectorSet(0, timeline ? -.49f : -.12f, -2.f, 0), level));
        panel.orientation = {orientation.x, orientation.y, orientation.z, orientation.w};
        panel.position = {head.pose.position.x + offset.x, head.pose.position.y + offset.y, head.pose.position.z + offset.z};
        placedSurface = surface;
        return true;
    }

    std::optional<XMFLOAT2> pointAtPanel(unsigned hand, XrTime time, bool insidePanel = true) const {
        XrSpaceLocation aim{XR_TYPE_SPACE_LOCATION};
        constexpr auto poseValid = XR_SPACE_LOCATION_POSITION_VALID_BIT | XR_SPACE_LOCATION_ORIENTATION_VALID_BIT;
        if (XR_FAILED(xrLocateSpace(aims[hand], local, time, &aim)) || (aim.locationFlags & poseValid) != poseValid)
            return std::nullopt;
        const auto panelRotation = rotation(panel.orientation);
        const auto aimRotation = rotation(aim.pose.orientation);
        const auto position = XMVectorSet(aim.pose.position.x - panel.position.x, aim.pose.position.y - panel.position.y,
            aim.pose.position.z - panel.position.z, 0);
        const auto direction = XMVector3Rotate(XMVectorSet(0, 0, -1, 0), XMLoadFloat4(&aimRotation));
        const auto inverse = XMQuaternionConjugate(XMLoadFloat4(&panelRotation));
        return VrUi::aim(XMVector3Rotate(position, inverse), XMVector3Rotate(direction, inverse), insidePanel, placedSurface);
    }

    std::optional<XMFLOAT2> readCommands(XrTime time, ViewSettings& view, const std::function<VrUi::Surface()>& surface,
               const std::function<void(VrCommand)>& invoke,
               const std::function<void(const VrUi::Pointer&)>& point) {
        XrActiveActionSet active{actions, XR_NULL_PATH};
        XrActionsSyncInfo sync{XR_TYPE_ACTIONS_SYNC_INFO};
        sync.countActiveActionSets = 1;
        sync.activeActionSets = &active;
        const auto result = xrSyncActions(session, &sync);
        if (result == XR_SESSION_NOT_FOCUSED) {
            grabbing = -1;
            stickStep = 0;
            triggerHeld.fill(true);
            gripHeld.fill(true);
            confirmPress.requireRelease();
            fileGesture = {};
            capturedPointer = -1;
            capturedTrigger = -1;
            point({std::nullopt, false, VrUi::Trigger::Cancelled});
            return std::nullopt;
        }
        check(result, "Lecture des commandes Touch");
        const auto button = [&](XrAction action, XrPath hand) {
            XrActionStateGetInfo info{XR_TYPE_ACTION_STATE_GET_INFO, nullptr, action, hand};
            XrActionStateBoolean value{XR_TYPE_ACTION_STATE_BOOLEAN};
            if (XR_FAILED(xrGetActionStateBoolean(session, &info, &value)) || !value.isActive)
                value.currentState = XR_FALSE;
            return value;
        };
        const auto pressed = [&](XrAction action, XrPath hand) {
            const auto value = button(action, hand);
            return value.changedSinceLastSync && value.currentState;
        };
        const auto pressure = [&](XrAction action, XrPath hand) {
            XrActionStateGetInfo info{XR_TYPE_ACTION_STATE_GET_INFO, nullptr, action, hand};
            XrActionStateFloat value{XR_TYPE_ACTION_STATE_FLOAT};
            return XR_SUCCEEDED(xrGetActionStateFloat(session, &info, &value)) && value.isActive ? value.currentState : 0.f;
        };
        XrVector2f axis{};
        int fileDirection = 0;
        bool horizontalHeld = false;
        std::array<bool, 2> gripPressed{};
        bool selectHeld = false, selectPressed = false, backPressed = false;
        for (unsigned i = 0; i < hands.size(); ++i) {
            backPressed |= pressed(back, hands[i]);
            const auto selection = button(select, hands[i]);
            selectHeld |= selection.currentState != 0;
            selectPressed |= selection.changedSinceLastSync && selection.currentState;
            if (pressed(center, hands[i])) invoke(VrCommand::Recenter);
            XrActionStateGetInfo joyInfo{XR_TYPE_ACTION_STATE_GET_INFO, nullptr, stick, hands[i]};
            XrActionStateVector2f joy{XR_TYPE_ACTION_STATE_VECTOR2F};
            if (XR_SUCCEEDED(xrGetActionStateVector2f(session, &joyInfo, &joy)) && joy.isActive) {
                if (std::abs(joy.currentState.y) > std::abs(axis.y)) axis.y = joy.currentState.y;
                if (std::abs(joy.currentState.x) > std::abs(axis.x)) axis.x = joy.currentState.x;
            }
            const bool gripDown = pressure(squeeze, hands[i]) > (gripHeld[i] ? .35f : .60f);
            gripPressed[i] = gripDown && !gripHeld[i];
            gripHeld[i] = gripDown;
            triggerHeld[i] = pressure(trigger, hands[i]) > (triggerHeld[i] ? .35f : .60f);
            const auto x = joy.currentState.x, y = joy.currentState.y;
            if (joy.isActive && std::abs(x) > std::abs(y) && std::abs(x) > .60f) {
                horizontalHeld = true;
                if (gripDown) fileDirection = x > 0 ? 1 : -1;
            }
        }
        const bool anyTriggerHeld = triggerHeld[0] || triggerHeld[1];
        const bool activate = confirmPress.update(selectHeld, anyTriggerHeld);
        const bool openedTimeline = !backPressed && surface() == VrUi::Surface::None && activate && anyTriggerHeld;
        if (backPressed) invoke(VrCommand::Back);
        else if (openedTimeline) invoke(VrCommand::Timeline);
        const auto current = surface();
        const bool fullMenu = current == VrUi::Surface::Full;
        const int changeFile = fileGesture.update(fullMenu ? 0 : fileDirection, horizontalHeld);
        const bool horizontal = std::abs(axis.x) > std::abs(axis.y);
        const bool seeking = !fullMenu && horizontal;
        const float direction = horizontal ? axis.x : fullMenu ? -axis.y : 0.f;
        const int step = direction > .60f ? 1 : direction < -.60f ? -1 : 0;
        const int gesture = fullMenu ? step * (horizontal ? 3 : 1) : step * 2;
        const auto now = std::chrono::steady_clock::now();
        const bool changedMenu = openedTimeline || backPressed;
        if (changeFile && !changedMenu) invoke(changeFile < 0 ? VrCommand::PreviousFile : VrCommand::NextFile);
        if (!changedMenu && gesture && (fullMenu || !fileGesture.consumesSeek()) &&
            (gesture != stickStep || ((!fullMenu || !horizontal) && now >= repeatAt))) {
            if (fullMenu && horizontal) invoke(step < 0 ? VrCommand::FocusSources : VrCommand::FocusFiles);
            else if (fullMenu) invoke(step < 0 ? VrCommand::Previous : VrCommand::Next);
            else invoke(step < 0 ? VrCommand::Rewind : VrCommand::Advance);
            const auto delay = fullMenu ? (gesture != stickStep ? 350 : 85) : 550;
            repeatAt = now + std::chrono::milliseconds(delay);
        }
        stickStep = gesture;
        if (changedMenu || changeFile) capturedPointer = capturedTrigger = -1;
        std::optional<XMFLOAT2> cursor;
        const auto panelSurface = surface();
        if (panelSurface != VrUi::Surface::None) {
            if (!placePanel(time, panelSurface)) return std::nullopt;
            if (capturedPointer >= 0) {
                const auto pointer = pointAtPanel(static_cast<unsigned>(capturedPointer), time, false);
                const bool held = triggerHeld[capturedTrigger];
                point({pointer, false, held ? VrUi::Trigger::Held : VrUi::Trigger::Released});
                if (!held) capturedPointer = capturedTrigger = -1;
                if (pointer && pointer->x >= 0 && pointer->x < 1 && pointer->y >= 0 && pointer->y < 1) cursor = pointer;
            } else {
                cursor = pointAtPanel(pointingHand, time);
                if (!cursor) {
                    const unsigned other = 1 - pointingHand;
                    cursor = pointAtPanel(other, time);
                    if (cursor) pointingHand = other;
                }
                const bool confirm = activate && !changedMenu;
                const bool triggerPress = confirm && anyTriggerHeld;
                if (triggerPress && cursor) {
                    capturedPointer = static_cast<int>(pointingHand);
                    capturedTrigger = triggerHeld[pointingHand] ? capturedPointer : 1 - capturedPointer;
                }
                point({cursor, confirm, triggerPress ? VrUi::Trigger::Pressed : VrUi::Trigger::Idle});
            }
        } else {
            capturedPointer = capturedTrigger = -1;
            if (selectPressed && !changedMenu && !changeFile) invoke(VrCommand::Select);
            placedSurface = VrUi::Surface::None;
        }
        if (fullMenu || changedMenu || fileGesture.consumesSeek()) { grabbing = -1; return cursor; }
        if (!seeking && std::abs(axis.y) > .30f)
            view.zoom = std::clamp(view.zoom * std::exp(axis.y * .012f), .3f, 3.f);
        if (grabbing >= 0 && !gripHeld[grabbing]) grabbing = -1;
        int hand = grabbing;
        if (hand < 0) for (int i = 0; i < 2; ++i) if (gripPressed[i]) { hand = i; break; }
        if (hand < 0) return cursor;

        XrSpaceLocation location{XR_TYPE_SPACE_LOCATION};
        if (XR_FAILED(xrLocateSpace(grips[hand], local, time, &location)) ||
            !(location.locationFlags & XR_SPACE_LOCATION_ORIENTATION_VALID_BIT)) { grabbing = -1; return cursor; }
        const auto next = rotation(location.pose.orientation);
        if (grabbing < 0) {
            grabbing = hand;
            startHand = next;
            startView = orientation;
        } else {
            auto delta = XMQuaternionMultiply(XMQuaternionInverse(XMLoadFloat4(&startHand)), XMLoadFloat4(&next));
            XMStoreFloat4(&orientation, XMQuaternionNormalize(XMQuaternionMultiply(XMLoadFloat4(&startView), delta)));
        }
        return cursor;
    }
};

Vr::Vr() : state_(std::make_unique<State>()) {
    auto& s = *state_;
    const char* extensions[] = {XR_KHR_D3D11_ENABLE_EXTENSION_NAME};
    XrInstanceCreateInfo info{XR_TYPE_INSTANCE_CREATE_INFO};
    strcpy_s(info.applicationInfo.applicationName, "Lecteur vidéo VR");
    info.applicationInfo.applicationVersion = 1;
    info.applicationInfo.apiVersion = XR_MAKE_VERSION(1, 0, 0);
    info.enabledExtensionCount = 1;
    info.enabledExtensionNames = extensions;
    s.check(xrCreateInstance(&info, &s.instance), "Initialisation d’OpenXR");
    XrSystemGetInfo system{XR_TYPE_SYSTEM_GET_INFO};
    system.formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY;
    s.check(xrGetSystem(s.instance, &system, &s.system), "Détection du casque VR");
    PFN_xrVoidFunction pointer = nullptr;
    s.check(xrGetInstanceProcAddr(s.instance, "xrGetD3D11GraphicsRequirementsKHR", &pointer), "Prise en charge de Direct3D");
    const auto getRequirements = reinterpret_cast<PFN_xrGetD3D11GraphicsRequirementsKHR>(pointer);
    s.check(getRequirements(s.instance, s.system, &s.requirements), "Carte graphique OpenXR");
    s.initActions();
}

Vr::~Vr() = default;
const LUID& Vr::adapter() const { return state_->requirements.adapterLuid; }
bool Vr::active() const { return state_->session != XR_NULL_HANDLE; }
void Vr::close() { state_->close(); }
void Vr::recenter() { state_->pendingCenter = true; state_->placedSurface = VrUi::Surface::None; state_->grabbing = -1; }

std::string Vr::runtime() const {
    XrInstanceProperties properties{XR_TYPE_INSTANCE_PROPERTIES};
    state_->check(xrGetInstanceProperties(state_->instance, &properties), "Runtime OpenXR");
    return properties.runtimeName;
}

void Vr::start(Renderer& renderer) {
    if (active()) return;
    auto& s = *state_;
    try {
        s.poll();
        XrGraphicsBindingD3D11KHR graphics{XR_TYPE_GRAPHICS_BINDING_D3D11_KHR};
        graphics.device = renderer.device();
        XrSessionCreateInfo info{XR_TYPE_SESSION_CREATE_INFO, &graphics};
        info.systemId = s.system;
        s.check(xrCreateSession(s.instance, &info, &s.session), "Connexion au Rift");
        XrSessionActionSetsAttachInfo attach{XR_TYPE_SESSION_ACTION_SETS_ATTACH_INFO};
        attach.countActionSets = 1;
        attach.actionSets = &s.actions;
        s.check(xrAttachSessionActionSets(s.session, &attach), "Activation des manettes");
        for (unsigned i = 0; i < s.hands.size(); ++i) {
            XrActionSpaceCreateInfo grip{XR_TYPE_ACTION_SPACE_CREATE_INFO};
            grip.action = s.gripPose;
            grip.subactionPath = s.hands[i];
            grip.poseInActionSpace = identity;
            s.check(xrCreateActionSpace(s.session, &grip, &s.grips[i]), "Suivi de la manette Touch");
            grip.action = s.aimPose;
            s.check(xrCreateActionSpace(s.session, &grip, &s.aims[i]), "Pointeur de la manette Touch");
        }
        s.makeSpace(XR_REFERENCE_SPACE_TYPE_LOCAL, s.local);
        s.makeSpace(XR_REFERENCE_SPACE_TYPE_VIEW, s.viewer);
        uint32_t count = 0;
        s.check(xrEnumerateSwapchainFormats(s.session, 0, &count, nullptr), "Formats VR");
        std::vector<int64_t> formats(count);
        s.check(xrEnumerateSwapchainFormats(s.session, count, &count, formats.data()), "Formats du Rift");
        const DXGI_FORMAT allowed[] = {DXGI_FORMAT_R8G8B8A8_UNORM_SRGB, DXGI_FORMAT_B8G8R8A8_UNORM_SRGB,
            DXGI_FORMAT_R8G8B8A8_UNORM, DXGI_FORMAT_B8G8R8A8_UNORM};
        for (auto format : allowed) if (std::find(formats.begin(), formats.end(), format) != formats.end()) { s.format = format; break; }
        if (s.format == DXGI_FORMAT_UNKNOWN) throw std::runtime_error("Aucun format Direct3D compatible avec le casque.");
        std::array<XrViewConfigurationView, 2> views{{{XR_TYPE_VIEW_CONFIGURATION_VIEW}, {XR_TYPE_VIEW_CONFIGURATION_VIEW}}};
        s.check(xrEnumerateViewConfigurationViews(s.instance, s.system, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO,
            static_cast<uint32_t>(views.size()), &count, views.data()), "Résolution des yeux");
        if (count != views.size()) throw std::runtime_error("Le casque doit proposer deux vues stéréoscopiques.");
        for (unsigned i = 0; i < views.size(); ++i)
            s.makeSwapchain(renderer.device(), s.eyes[i], views[i].recommendedImageRectWidth, views[i].recommendedImageRectHeight);
        s.makeSwapchain(renderer.device(), s.menuSurface, VrUi::width, VrUi::height);
        recenter();
    } catch (...) {
        s.close();
        throw;
    }
}

bool Vr::frame(Renderer& renderer, const Media& media, ViewSettings& view,
               const std::function<void(VrCommand)>& input, const std::function<VrUi::Surface()>& surface,
               const std::function<void(const VrUi::Pointer&)>& point,
               const std::function<ID3D11ShaderResourceView*()>& menu) {
    auto& s = *state_;
    s.poll();
    if (!s.running) return false;
    XrFrameWaitInfo wait{XR_TYPE_FRAME_WAIT_INFO};
    XrFrameState frame{XR_TYPE_FRAME_STATE};
    s.check(xrWaitFrame(s.session, &wait, &frame), "Synchronisation avec le Rift");
    XrFrameBeginInfo begin{XR_TYPE_FRAME_BEGIN_INFO};
    s.check(xrBeginFrame(s.session, &begin), "Démarrage de l’image VR");
    std::array<const XrCompositionLayerBaseHeader*, 2> layers{};
    XrFrameEndInfo end{XR_TYPE_FRAME_END_INFO};
    end.displayTime = frame.predictedDisplayTime;
    end.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
    end.layers = layers.data();
    std::array<XrCompositionLayerProjectionView, 2> projectionViews{};
    XrCompositionLayerProjection projection{XR_TYPE_COMPOSITION_LAYER_PROJECTION};
    XrCompositionLayerQuad overlay{XR_TYPE_COMPOSITION_LAYER_QUAD};
    try {
        if (frame.shouldRender) {
            std::array<XrView, 2> eyes{{{XR_TYPE_VIEW}, {XR_TYPE_VIEW}}};
            XrViewState state{XR_TYPE_VIEW_STATE};
            XrViewLocateInfo locate{XR_TYPE_VIEW_LOCATE_INFO};
            locate.viewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
            locate.displayTime = frame.predictedDisplayTime;
            locate.space = s.local;
            uint32_t count = 0;
            s.check(xrLocateViews(s.session, &locate, &state, static_cast<uint32_t>(eyes.size()), &count, eyes.data()), "Suivi de la tête");
            if (count == eyes.size() && (state.viewStateFlags & XR_VIEW_STATE_ORIENTATION_VALID_BIT)) {
                if (s.pendingCenter) {
                    auto q = rotation(eyes[0].pose.orientation);
                    XMFLOAT3 forward;
                    XMStoreFloat3(&forward, XMVector3Rotate(XMVectorSet(0, 0, -1, 0), XMLoadFloat4(&q)));
                    const float heading = std::atan2(-forward.x, -forward.z);
                    XMStoreFloat4(&s.orientation, XMQuaternionRotationRollPitchYaw(0, heading, 0));
                    s.pendingCenter = false;
                }
                auto pointer = s.readCommands(frame.predictedDisplayTime, view, surface, input, point);
                const bool srgb = s.format == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB || s.format == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB;
                for (unsigned i = 0; i < count; ++i) {
                    const auto& eye = eyes[i];
                    auto& swap = s.eyes[i];
                    const Camera camera{rotation(eye.pose.orientation), std::tan(eye.fov.angleLeft),
                        std::tan(eye.fov.angleRight), std::tan(eye.fov.angleDown), std::tan(eye.fov.angleUp)};
                    s.render(swap, [&](auto* target) {
                        renderer.draw(target, swap.width, swap.height, camera, media, view, s.orientation, i, srgb);
                    });
                    projectionViews[i] = {XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW, nullptr, eye.pose, eye.fov, s.subImage(swap)};
                }
                projection.space = s.local;
                projection.viewCount = count;
                projection.views = projectionViews.data();
                layers[end.layerCount++] = reinterpret_cast<const XrCompositionLayerBaseHeader*>(&projection);
                const auto current = surface();
                if (auto* texture = current != VrUi::Surface::None && s.placePanel(frame.predictedDisplayTime, current) ? menu() : nullptr) {
                    const auto height = VrUi::surfacePixels(current);
                    if (pointer) pointer->y *= static_cast<float>(height) / VrUi::height;
                    s.render(s.menuSurface, [&](auto* target) {
                        renderer.blit(target, VrUi::width, VrUi::height, texture, srgb, pointer ? &*pointer : nullptr);
                    });
                    overlay.space = s.local;
                    overlay.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
                    overlay.subImage = s.subImage(s.menuSurface);
                    overlay.subImage.imageRect.extent.height = height;
                    overlay.pose = s.panel;
                    overlay.size = {VrUi::surfaceWidth(current), VrUi::surfaceHeight(current)};
                    layers[end.layerCount++] = reinterpret_cast<const XrCompositionLayerBaseHeader*>(&overlay);
                }
                if (current == VrUi::Surface::None) s.placedSurface = VrUi::Surface::None;
            }
        }
    } catch (...) {
        end.layerCount = 0;
        xrEndFrame(s.session, &end);
        throw;
    }
    s.check(xrEndFrame(s.session, &end), "Envoi de l’image au casque");
    return true;
}
