/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

"use strict";

const Services = require("Services");
const ElementStyle = require("devtools/client/inspector/rules/models/element-style");
const { createFactory, createElement } = require("devtools/client/shared/vendor/react");
const { Provider } = require("devtools/client/shared/vendor/react-redux");

const { updateRules } = require("./actions/rules");

const RulesApp = createFactory(require("./components/RulesApp"));

const { LocalizationHelper } = require("devtools/shared/l10n");
const INSPECTOR_L10N =
  new LocalizationHelper("devtools/client/locales/inspector.properties");

const PREF_UA_STYLES = "devtools.inspector.showUserAgentStyles";

class RulesView {
  constructor(inspector, window) {
    this.cssProperties = inspector.cssProperties;
    this.doc = window.document;
    this.inspector = inspector;
    this.pageStyle = inspector.pageStyle;
    this.selection = inspector.selection;
    this.store = inspector.store;
    this.toolbox = inspector.toolbox;

    this.showUserAgentStyles = Services.prefs.getBoolPref(PREF_UA_STYLES);

    this.onSelection = this.onSelection.bind(this);

    this.inspector.sidebar.on("select", this.onSelection);
    this.selection.on("detached-front", this.onSelection);
    this.selection.on("new-node-front", this.onSelection);

    this.init();
  }

  init() {
    if (!this.inspector) {
      return;
    }

    const rulesApp = RulesApp({});

    const provider = createElement(Provider, {
      id: "ruleview",
      key: "ruleview",
      store: this.store,
      title: INSPECTOR_L10N.getStr("inspector.sidebar.ruleViewTitle"),
    }, rulesApp);

    // Exposes the provider to let inspector.js use it in setupSidebar.
    this.provider = provider;
  }

  destroy() {
    this.inspector.sidebar.off("select", this.onSelection);
    this.selection.off("detached-front", this.onSelection);
    this.selection.off("new-node-front", this.onSelection);

    if (this.elementStyle) {
      this.elementStyle.destroy();
    }

    this._dummyElement = null;
    this.cssProperties = null;
    this.doc = null;
    this.elementStyle = null;
    this.inspector = null;
    this.pageStyle = null;
    this.selection = null;
    this.showUserAgentStyles = null;
    this.store = null;
    this.toolbox = null;
  }

  /**
   * Creates a dummy element in the document that helps get the computed style in
   * TextProperty.
   *
   * @return {Element} used to get the computed style for text properties.
   */
  get dummyElement() {
    // To figure out how shorthand properties are interpreted by the
    // engine, we will set properties on a dummy element and observe
    // how their .style attribute reflects them as computed values.
    if (!this._dummyElement) {
      this._dummyElement = this.doc.createElement("div");
    }

    return this._dummyElement;
  }

  /**
   * Returns true if the rules panel is visible, and false otherwise.
   */
  isPanelVisible() {
    return this.inspector && this.inspector.toolbox && this.inspector.sidebar &&
           this.inspector.toolbox.currentToolId === "inspector" &&
           this.inspector.sidebar.getCurrentTabID() === "newruleview";
  }

  /**
   * Handler for selection events "detached-front" and "new-node-front" and inspector
   * sidbar "select" event. Updates the rules view with the selected node if the panel
   * is visible.
   */
  onSelection() {
    if (!this.isPanelVisible()) {
      return;
    }

    if (!this.selection.isConnected() ||
        !this.selection.isElementNode()) {
      this.update();
      return;
    }

    this.update(this.selection.nodeFront);
  }

  /**
   * Updates the rules view by dispatching the new rules data of the newly selected
   * element. This is called when the rules view becomes visible or upon new node
   * selection.
   *
   * @param  {NodeFront|null} element
   *         The NodeFront of the current selected element.
   */
  async update(element) {
    if (!element) {
      this.store.dispatch(updateRules([]));
      return;
    }

    this.elementStyle = new ElementStyle(element, this, {}, this.pageStyle,
      this.showUserAgentStyles);
    await this.elementStyle.populate();

    this.store.dispatch(updateRules(this.elementStyle.rules));
  }
}

module.exports = RulesView;
