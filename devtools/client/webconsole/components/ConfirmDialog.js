/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

"use strict";

loader.lazyRequireGetter(this, "PropTypes", "devtools/client/shared/vendor/react-prop-types");
loader.lazyRequireGetter(this, "gDevTools", "devtools/client/framework/devtools", true);
loader.lazyRequireGetter(this, "HTMLTooltip", "devtools/client/shared/widgets/tooltip/HTMLTooltip", true);
loader.lazyRequireGetter(this, "createPortal", "devtools/client/shared/vendor/react-dom", true);

// React & Redux
const { Component } = require("devtools/client/shared/vendor/react");
const dom = require("devtools/client/shared/vendor/react-dom-factories");
const { connect } = require("devtools/client/shared/vendor/react-redux");

const {getAutocompleteState} = require("devtools/client/webconsole/selectors/autocomplete");
const autocompleteActions = require("devtools/client/webconsole/actions/autocomplete");
const { l10n } = require("devtools/client/webconsole/utils/messages");

class ConfirmDialog extends Component {
  static get propTypes() {
    return {
      // Console object.
      hud: PropTypes.object.isRequired,
      // Update autocomplete popup state.
      autocompleteUpdate: PropTypes.func.isRequired,
      autocompleteClear: PropTypes.func.isRequired,
      // Data to be displayed in the confirm dialog.
      getterPath: PropTypes.array.isRequired,
      serviceContainer: PropTypes.object.isRequired,
    };
  }

  constructor(props) {
    super(props);

    const { hud } = props;
    hud.confirmDialog = this;

    this.cancel = this.cancel.bind(this);
    this.confirm = this.confirm.bind(this);
  }

  componentDidMount() {
    const doc = this.props.hud.document;
    const toolbox = gDevTools.getToolbox(this.props.hud.owner.target);
    const tooltipDoc = toolbox ? toolbox.doc : doc;
    // The popup will be attached to the toolbox document or HUD document in the case
    // such as the browser console which doesn't have a toolbox.
    this.tooltip = new HTMLTooltip(tooltipDoc, {
      className: "invoke-confirm",
    });
  }

  componentDidUpdate() {
    const {getterPath, serviceContainer} = this.props;

    if (getterPath) {
      this.tooltip.show(serviceContainer.getJsTermTooltipAnchor(), {y: 5});
      this.tooltip.focus();
    } else {
      this.tooltip.hide();
      this.props.hud.jsterm.focus();
    }
  }

  componentDidThrow(e) {
    console.error("Error in ConfirmDialog", e);
    this.setState(state => ({...state, hasError: true}));
  }

  cancel() {
    this.tooltip.hide();
    this.props.autocompleteClear();
  }

  confirm() {
    this.tooltip.hide();
    this.props.autocompleteUpdate(this.props.getterPath);
  }

  render() {
    if (
      (this.state && this.state.hasError) ||
      (!this.props || !this.props.getterPath)
    ) {
      return null;
    }

    const {getterPath} = this.props;
    const getterName = getterPath.join(".");

    // We deliberately use getStr, and not getFormatStr, because we want getterName to
    // be wrapped in its own span.
    const description = l10n.getStr("webconsole.confirmDialog.getter.label");
    const [descriptionPrefix, descriptionSuffix] = description.split("%S");

    return createPortal([
      dom.p({
        className: "confirm-label",
      },
        dom.span({}, descriptionPrefix),
        dom.span({className: "emphasized"}, getterName),
        dom.span({}, descriptionSuffix)
      ),
      dom.button({
        className: "confirm-button",
        onBlur: () => this.cancel(),
        onKeyDown: event => {
          const {key} = event;
          if (["Escape", "ArrowLeft", "Backspace"].includes(key)) {
            this.cancel();
            event.stopPropagation();
            return;
          }

          if (["Tab", "Enter", " "].includes(key)) {
            this.confirm();
            event.stopPropagation();
          }
        },
        // We can't use onClick because it would respond to Enter and Space keypress.
        // We don't want that because we have a Ctrl+Space shortcut to force an
        // autocomplete update; if the ConfirmDialog need to be displayed, since
        // we automatically focus the button, the keyup on space would fire the onClick
        // handler.
        onMouseDown: this.confirm,
      }, l10n.getStr("webconsole.confirmDialog.getter.confirmButtonLabel")),
    ], this.tooltip.panel);
  }
}

// Redux connect
function mapStateToProps(state) {
  const autocompleteData = getAutocompleteState(state);
  return {
    getterPath: autocompleteData.getterPath,
  };
}

function mapDispatchToProps(dispatch) {
  return {
    autocompleteUpdate: getterPath =>
      dispatch(autocompleteActions.autocompleteUpdate(true, getterPath)),
    autocompleteClear: () => dispatch(autocompleteActions.autocompleteClear()),
  };
}

module.exports = connect(mapStateToProps, mapDispatchToProps)(ConfirmDialog);
