/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

"use strict";

const { createFactory, PureComponent } = require("devtools/client/shared/vendor/react");
const dom = require("devtools/client/shared/vendor/react-dom-factories");
const PropTypes = require("devtools/client/shared/vendor/react-prop-types");

const FluentReact = require("devtools/client/shared/vendor/fluent-react");
const Localized = createFactory(FluentReact.Localized);

const ErrorMessage = createFactory(require("../shared/ErrorMessage"));

const Actions = require("../../actions/index");

/**
 * This component provides an installer for temporary extension.
 */
class TemporaryExtensionInstaller extends PureComponent {
  static get propTypes() {
    return {
      dispatch: PropTypes.func.isRequired,
      temporaryInstallError: PropTypes.string,
    };
  }

  install() {
    this.props.dispatch(Actions.installTemporaryExtension());
  }

  render() {
    const { temporaryInstallError } = this.props;
    return dom.div(
      {},
      Localized(
        {
          id: "about-debugging-tmp-extension-install-button",
        },
        dom.button(
          {
            className: "default-button js-temporary-extension-install-button",
            onClick: e => this.install(),
          },
          "Load Temporary Add-on…"
        )
      ),
      temporaryInstallError ? ErrorMessage(
        {
          errorDescriptionKey: "about-debugging-tmp-extension-install-error",
        },
        dom.div(
          {
            className: "technical-text",
          },
          temporaryInstallError
        )
      ) : null
    );
  }
}

module.exports = TemporaryExtensionInstaller;
