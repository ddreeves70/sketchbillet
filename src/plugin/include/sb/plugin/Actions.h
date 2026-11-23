#pragma once

/// @file
/// Actions: toolbar buttons and dropdown items that run a command by name, and
/// the toolbar's tabs.

#include <functional>
#include <string>
#include <vector>

#include "sb/model/CommandRegistry.h"

namespace sb::plugin {

/// A toolbar button or dropdown item that runs a command, or the plugin's own code.
///
/// Actions are plain data, with no Qt, so any plugin can add them and every
/// front end shows them its own way: the desktop app on its toolbar, in a tab
/// and a panel (`menu`, "Model/Create"), `sketchbillet_cli plugins` as a list.
/// Every action is in its panel's dropdown (the panel's name, like Create ▾);
/// one with `toolbar` set is also a button on the panel.
///
/// Changes to a design always go through a `command`, so undo works. Things
/// that aren't changes to a design, like a setting, use `run` instead, often
/// with `checked` for a check mark:
/// @code
/// ctx.addAction({.id = "example.ruler.inches", .text = "Ruler shows inches", .menu = "View/Display",
///                .run = [&ctx] { auto& s = ctx.settings(); s.set("unit", s.text("unit", "in") == "in" ? "mm"
///                : "in"); }, .checked = [&ctx] { return ctx.settings().text("unit", "in") == "in"; }});
/// @endcode
///
/// @code
/// ctx.addAction({.id = "sb.modeling.box", .text = "Box", .menu = "Model/Create",
///                .command = "feature.add_box", .args = {{"size", kernel::Vec3{20, 20, 20}}},
///                .toolbar = true, .tip = "Add a 20 mm box. Change its size in Properties."});
/// ctx.addAction({.id = "sb.modeling.cut", .text = "Cut", .menu = "Model/Combine",
///                .command = "feature.add_boolean", .args = {{"operation", std::string("cut")}},
///                .fromSelection = {"a", "b"}});
/// @endcode
struct ActionSpec {
    std::string id{};    ///< Stable id; an outside plugin's must start with its plugin id and a dot.
    std::string text{};  ///< The words in the dropdown, and on the button, e.g. "Box".
    /// Where it goes: the toolbar tab, then the panel in it, like "Model/Create"
    /// (the Create panel of the Model tab). Both are made if they don't exist yet,
    /// in the order first named. More parts make a submenu of the panel's dropdown:
    /// "View/Grid/Grid style". A single name, like "Tools", is a panel of that name
    /// on a tab of that name. Several places, split by ";", put it in each:
    /// "Model/Inspect;Inspect/Measure".
    std::string menu{};
    std::string command{};                     ///< The command it runs, e.g. "feature.add_box".
    model::Args args{};                        ///< Fixed arguments for the command.
    std::vector<std::string> fromSelection{};  ///< Arguments filled from the selected steps, in order.
                                               ///< The action is only enabled when exactly this many
                                               ///< steps are selected.
    /// An argument filled with every selected step, in the order picked (an
    /// ArgType::FeatureList argument). The action is enabled when one or more
    /// steps are selected. Use this or `fromSelection`, not both.
    std::string fromSelectionList{};
    /// An argument filled with the picked edges (an ArgType::GeometryList
    /// argument), which must all be on one body; that body goes in `fromEdgesBody`.
    /// Clicking the action with no edges picked turns on picking edges
    /// (Selection::setPickMode()) and says to pick them, then click it again.
    std::string fromEdges{};
    /// With `fromEdges`: the argument the edges' body goes in, like "body".
    std::string fromEdgesBody{};
    /// Like `fromEdges`, for faces: an argument filled with the picked faces (an
    /// ArgType::GeometryList argument), all on one body. Clicking the action with
    /// no faces picked turns on picking faces and says to pick them.
    std::string fromFaces{};
    /// With `fromFaces`: the argument the faces' body goes in, like "body".
    std::string fromFacesBody{};
    /// An argument filled with the picked component's name (an ArgType::Text
    /// argument), in an assembly. The action is enabled when exactly one
    /// component is picked.
    std::string fromComponent{};
    /// An argument filled with the name of the picked component's part (an
    /// ArgType::Text argument), like "Wheel" for Wheel 2. The action is enabled
    /// when exactly one component is picked.
    std::string fromComponentPart{};
    /// An argument filled with a file the student picks (an ArgType::Text
    /// argument): clicking the action opens a file window, and the file's full
    /// path, as UTF-8, goes in this argument. Closing the window does nothing.
    std::string fromFile{};
    /// With `fromFile`: the kinds of file the window shows, as Qt writes them,
    /// like "SketchBillet designs (*.sb)". Empty shows every file.
    std::string fileTypes{};
    /// With `fromFile`: true to ask for a new file to save to (the window warns
    /// before replacing one), false to pick a file to open.
    bool saveFile = false;
    /// With `fromFile`: the file name the window starts with, like "Wheel.sb",
    /// asked each time. Empty starts with none.
    std::function<std::string()> suggestedFile{};
    std::string shortcut{};  ///< Keyboard shortcut, e.g. "Ctrl+B" or "Delete". May be empty.
    bool toolbar = false;    ///< True to also show it as a button on its panel, not only in the dropdown.
    /// Shorter words for the toolbar button, e.g. "Sketch" for "New sketch on XY
    /// (the ground)". Empty means the button uses `text`.
    std::string buttonText{};
    /// Which toolbar mode the item belongs to (see PluginContext::setToolbarMode()).
    /// While that mode is on, its shortcut wins over any other item's with the same
    /// keys (the other item keeps its keys the rest of the time), like Delete
    /// deleting what's picked in a sketch instead of the step. Which buttons show
    /// is up to the tabs (TabSpec::mode).
    std::string toolbarMode{};
    /// True for the one main button of its tab, drawn at the right end of the
    /// tab, in green, like Finish sketch. It isn't in a panel's dropdown.
    bool primary = false;
    /// A picture for the button and dropdown item: SVG text, drawn in a 24 by 24
    /// box (`viewBox="0 0 24 24"`). Write `currentColor` for lines that should follow
    /// the theme (dark on a light theme, light on a dark one). Empty, or text that
    /// isn't SVG SketchBillet can draw: words only. See "Icons" in the plugin guide.
    std::string icon{};
    std::string tip{};  ///< One plain sentence shown when the mouse rests on it.
    /// Runs the plugin's own code instead of a command, for things that aren't
    /// changes to a design (a setting, a dialog). Give either `command` or `run`.
    std::function<void()> run{};
    /// Opens a design file instead of running a command: gives the file's path
    /// (UTF-8), and SketchBillet opens it beside the open designs, in its own tab,
    /// or shows its tab if it's open already (Open part opens a linked part's file
    /// this way). An empty path opens nothing.
    std::function<std::string()> openFile{};
    /// If set, the item has a check mark, shown when this returns true. SketchBillet asks
    /// when the menu opens and after every change (SettingChanged, design changes).
    std::function<bool()> checked{};
    /// If set, the item is enabled only while this returns true (on top of the
    /// rules for `fromSelection`), and greyed out otherwise. SketchBillet asks when a menu
    /// opens, after design, selection and setting changes, and when a plugin sends
    /// ActionsChanged. Without it, an item that runs your code is always enabled.
    std::function<bool()> enabled{};
    /// If set, the words on the item come from this instead of `text`, asked at
    /// the same times as `checked`, e.g. "Other (12.5 mm)...".
    std::function<std::string()> currentText{};
    std::string plugin{};  ///< Set by SketchBillet: the id of the plugin that added it.
};

/// When a toolbar tab shows. Tabs are made by the actions in them
/// (ActionSpec::menu); a plugin only adds a TabSpec for a tab that comes and
/// goes with a toolbar mode, like the Sketch tab:
/// @code
/// ctx.addTab({.name = "Sketch", .mode = "sb.sketcher.editing"});
/// @endcode
/// SketchBillet puts its own tabs first, in this order: Model, Sketch, Inspect,
/// View. Other tabs follow, in the order first named.
struct TabSpec {
    std::string name{};  ///< The tab's name, as in ActionSpec::menu, like "Sketch".
    /// True (the default): the tab always shows. False: it shows only while
    /// `mode` is on (see PluginContext::setToolbarMode()), and is picked when the
    /// mode turns on. An empty `mode` means the usual toolbar, with no mode on:
    /// the Model tab hides while a sketch is being edited.
    bool always = true;
    std::string mode{};    ///< With `always` false, the toolbar mode the tab shows in.
    std::string plugin{};  ///< Set by SketchBillet: the id of the plugin that added it.
};

}  // namespace sb::plugin
