/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

"use strict";

const Services = require("Services");
const {
  createElement,
  createFactory,
  Fragment,
  PureComponent,
} = require("devtools/client/shared/vendor/react");
const dom = require("devtools/client/shared/vendor/react-dom-factories");
const PropTypes = require("devtools/client/shared/vendor/react-prop-types");
const { connect } = require("devtools/client/shared/vendor/react-redux");

const Accordion = createFactory(require("devtools/client/inspector/layout/components/Accordion"));
const Rule = createFactory(require("./Rule"));
const Rules = createFactory(require("./Rules"));
const Toolbar = createFactory(require("./Toolbar"));

const { getStr } = require("../utils/l10n");
const Types = require("../types");

const SHOW_PSEUDO_ELEMENTS_PREF = "devtools.inspector.show_pseudo_elements";

class RulesApp extends PureComponent {
  static get propTypes() {
    return {
      rules: PropTypes.arrayOf(PropTypes.shape(Types.rule)).isRequired,
    };
  }

  renderInheritedRules(rules) {
    if (!rules.length) {
      return null;
    }

    const output = [];
    let lastInherited;

    for (const rule of rules) {
      if (rule.inheritance.inherited !== lastInherited) {
        lastInherited = rule.inheritance.inherited;

        output.push(
          dom.div({ className: "ruleview-header" }, rule.inheritance.inheritedSource)
        );
      }

      output.push(Rule({ rule }));
    }

    return output;
  }

  renderKeyframesRules(rules) {
    if (!rules.length) {
      return null;
    }

    const output = [];
    let lastKeyframes;

    for (const rule of rules) {
      if (rule.keyframesRule.id === lastKeyframes) {
        continue;
      }

      lastKeyframes = rule.keyframesRule.id;

      const items = [
        {
          component: Rules,
          componentProps: {
            rules: rules.filter(r => r.keyframesRule.id === lastKeyframes),
          },
          header: rule.keyframesRule.keyframesName,
          opened: true,
        },
      ];

      output.push(Accordion({ items }));
    }

    return output;
  }

  renderStyleRules(rules) {
    if (!rules.length) {
      return null;
    }

    return Rules({ rules });
  }

  renderPseudoElementRules(rules) {
    if (!rules.length) {
      return null;
    }

    const items = [
      {
        component: Rules,
        componentProps: { rules },
        header: getStr("rule.pseudoElement"),
        opened: Services.prefs.getBoolPref(SHOW_PSEUDO_ELEMENTS_PREF),
        onToggled: () => {
          const opened = Services.prefs.getBoolPref(SHOW_PSEUDO_ELEMENTS_PREF);
          Services.prefs.setBoolPref(SHOW_PSEUDO_ELEMENTS_PREF, !opened);
        },
      },
    ];

    return createElement(Fragment, null,
      Accordion({ items }),
      dom.div({ className: "ruleview-header" }, getStr("rule.selectedElement"))
    );
  }

  render() {
    const { rules } = this.props.rules;
    const inheritedRules = [];
    const keyframesRules = [];
    const pseudoElementRules = [];
    const styleRules = [];

    for (const rule of rules) {
      if (rule.inheritance) {
        inheritedRules.push(rule);
      } else if (rule.keyframesRule) {
        keyframesRules.push(rule);
      } else if (rule.pseudoElement) {
        pseudoElementRules.push(rule);
      } else {
        styleRules.push(rule);
      }
    }

    return (
      dom.div(
        {
          id: "sidebar-panel-ruleview",
          className: "theme-sidebar inspector-tabpanel",
        },
        Toolbar({}),
        dom.div(
          {
            id: "ruleview-container",
            className: "ruleview",
          },
          dom.div(
            {
              id: "ruleview-container-focusable",
              tabIndex: -1,
            },
            rules.length > 0 ?
              createElement(Fragment, null,
                this.renderPseudoElementRules(pseudoElementRules),
                this.renderStyleRules(styleRules),
                this.renderInheritedRules(inheritedRules),
                this.renderKeyframesRules(keyframesRules)
              )
              :
              dom.div({ className: "devtools-sidepanel-no-result" },
                getStr("rule.empty")
              )
          )
        )
      )
    );
  }
}

module.exports = connect(state => state)(RulesApp);
