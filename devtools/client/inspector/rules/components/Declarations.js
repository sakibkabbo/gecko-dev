/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

"use strict";

const { createFactory, PureComponent } = require("devtools/client/shared/vendor/react");
const dom = require("devtools/client/shared/vendor/react-dom-factories");
const PropTypes = require("devtools/client/shared/vendor/react-prop-types");

const Declaration = createFactory(require("./Declaration"));

const Types = require("../types");

class Declarations extends PureComponent {
  static get propTypes() {
    return {
      declarations: PropTypes.arrayOf(PropTypes.shape(Types.declaration)).isRequired,
    };
  }

  render() {
    const { declarations } = this.props;

    if (!declarations.length) {
      return null;
    }

    return (
      dom.ul({ className: "ruleview-propertylist" },
        declarations.map(declaration => {
          return Declaration({
            key: declaration.id,
            declaration,
          });
        })
      )
    );
  }
}

module.exports = Declarations;
