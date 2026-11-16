#include "NativeMenuState.h"
#include "mc/client/gui/controls/UIControl.h"
#include "mc/client/gui/controls/VisualTree.h"
#include "mc/client/gui/screens/FocusManager.h"
#include "mc/client/gui/screens/ScreenView.h"
#include <cmath>

namespace sle::client {
namespace {
void visit(std::shared_ptr<UIControl> const& c, bool parentVisible, int depth, NativeMenuState& result) {
    if (!c || depth > 128) return;
    bool const visible = parentVisible && c->mIsVisibleInTree;
    if (!visible) return;
    auto const& name = c->mName.get();
    // ui_common.screen_panel.popup_dialog_factory creates these controls INSIDE
    // start_screen. The screen name and scene stack do not change when shown.
    if (vanilla::isInlineModal(name)) {
        result.modalVisible = true;
        result.modalName = name;
    }
    if (name == "play_button") {
        auto const size = c->mSize.get();
        if (size.x > 0 && size.y > 0 && c->mAlpha > 0.0f) {
            if (c->mCachedPositionDirty) c->_setCachedPosition();
            auto const pos = c->mCachedPosition.get();
            if (std::isfinite(pos.x) && std::isfinite(pos.y)) {
                result.playVisible = true;
                result.play = {pos.x,pos.y,pos.x+size.x,pos.y+size.y};
                if (auto row = c->mParent.lock(); row && row->mSize->y >= size.y)
                    result.rowStep = row->mSize->y;
            }
        }
    }
    for (auto const& child : c->mChildren.get()) visit(child, visible, depth+1, result);
}
bool popupAncestor(std::shared_ptr<UIControl> c) {
    for (int depth=0; c && depth<128; ++depth, c=c->mParent.lock()) {
        if (!c->mIsVisibleInTree) return false;
        auto const& name = c->mName.get();
        if (vanilla::isInlineModal(name) || name == "popup_dialog_factory") return true;
    }
    return false;
}
}
std::shared_ptr<UIControl> menuRoot(ScreenView& view) {
    return view.mVisualTree ? view.mVisualTree->mRootControl : nullptr;
}
NativeMenuState inspectMenuTree(std::shared_ptr<UIControl> const& root) {
    NativeMenuState result;
    visit(root, true, 0, result);
    return result;
}
NativeMenuState inspectMenu(ScreenView& view) {
    auto result = inspectMenuTree(menuRoot(view));
    if (view.mFocusManager) {
        if (auto modal=view.mFocusManager->mCurrentModalRoot.lock(); modal && popupAncestor(modal)) {
            result.focusModal = true;
            if (result.modalName.empty()) result.modalName=modal->mName.get();
        }
    }
    return result;
}
}
