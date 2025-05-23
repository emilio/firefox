/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

"use strict";

const {
  PureComponent,
} = require("resource://devtools/client/shared/vendor/react.mjs");
const dom = require("resource://devtools/client/shared/vendor/react-dom-factories.js");
const PropTypes = require("resource://devtools/client/shared/vendor/react-prop-types.mjs");
const {
  connect,
} = require("resource://devtools/client/shared/vendor/react-redux.js");

const {
  getStr,
} = require("resource://devtools/client/responsive/utils/l10n.js");

loader.lazyRequireGetter(
  this,
  "showMenu",
  "resource://devtools/client/shared/components/menu/utils.js",
  true
);

class SettingsMenu extends PureComponent {
  static get propTypes() {
    return {
      leftAlignmentEnabled: PropTypes.bool.isRequired,
      onToggleLeftAlignment: PropTypes.func.isRequired,
      onToggleReloadOnTouchSimulation: PropTypes.func.isRequired,
      onToggleReloadOnUserAgent: PropTypes.func.isRequired,
      onToggleUserAgentInput: PropTypes.func.isRequired,
      reloadOnTouchSimulation: PropTypes.bool.isRequired,
      reloadOnUserAgent: PropTypes.bool.isRequired,
      showUserAgentInput: PropTypes.bool.isRequired,
    };
  }

  constructor(props) {
    super(props);
    this.onToggleSettingMenu = this.onToggleSettingMenu.bind(this);
  }

  onToggleSettingMenu(event) {
    const {
      leftAlignmentEnabled,
      onToggleLeftAlignment,
      onToggleReloadOnTouchSimulation,
      onToggleReloadOnUserAgent,
      onToggleUserAgentInput,
      reloadOnTouchSimulation,
      reloadOnUserAgent,
      showUserAgentInput,
    } = this.props;

    const menuItems = [
      {
        id: "toggleLeftAlignment",
        checked: leftAlignmentEnabled,
        label: getStr("responsive.leftAlignViewport"),
        type: "checkbox",
        click: () => {
          onToggleLeftAlignment();
        },
      },
      "-",
      {
        id: "toggleUserAgentInput",
        checked: showUserAgentInput,
        label: getStr("responsive.showUserAgentInput"),
        type: "checkbox",
        click: () => {
          onToggleUserAgentInput();
        },
      },
      "-",
      {
        id: "touchSimulation",
        checked: reloadOnTouchSimulation,
        label: getStr("responsive.reloadConditions.touchSimulation"),
        type: "checkbox",
        click: () => {
          onToggleReloadOnTouchSimulation();
        },
      },
      {
        id: "userAgent",
        checked: reloadOnUserAgent,
        label: getStr("responsive.reloadConditions.userAgent"),
        type: "checkbox",
        click: () => {
          onToggleReloadOnUserAgent();
        },
      },
    ];

    showMenu(menuItems, {
      button: event.target,
    });
  }

  render() {
    return dom.button({
      id: "settings-button",
      className: "devtools-button",
      onClick: this.onToggleSettingMenu,
    });
  }
}

const mapStateToProps = state => {
  return {
    leftAlignmentEnabled: state.ui.leftAlignmentEnabled,
    reloadOnTouchSimulation: state.ui.reloadOnTouchSimulation,
    reloadOnUserAgent: state.ui.reloadOnUserAgent,
    showUserAgentInput: state.ui.showUserAgentInput,
  };
};

module.exports = connect(mapStateToProps)(SettingsMenu);
