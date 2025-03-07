/* globals ContentSearchUIController */
"use strict";

import {actionCreators as ac, actionTypes as at} from "common/Actions.jsm";
import {FormattedMessage, injectIntl} from "react-intl";
import {connect} from "react-redux";
import {IS_NEWTAB} from "content-src/lib/constants";
import React from "react";

export class _Search extends React.PureComponent {
  constructor(props) {
    super(props);
    this.onSearchClick = this.onSearchClick.bind(this);
    this.onSearchHandoffClick = this.onSearchHandoffClick.bind(this);
    this.onInputMount = this.onInputMount.bind(this);
  }

  handleEvent(event) {
    // Also track search events with our own telemetry
    if (event.detail.type === "Search") {
      this.props.dispatch(ac.UserEvent({event: "SEARCH"}));
    }
  }

  onSearchClick(event) {
    window.gContentSearchController.search(event);
  }

  onSearchHandoffClick(event) {
    // When search hand-off is enabled, we render a big button that is styled to
    // look like a search textbox. If the button is clicked with the mouse, we style
    // the button as if it was a focused search box and show a fake cursor but
    // really focus the awesomebar without the focus styles.
    // If the button is clicked from the keyboard, we focus the awesomebar normally.
    // This is to minimize confusion with users navigating with the keyboard and
    // users using assistive technologoy.
    const isKeyboardClick = event.clientX === 0 && event.clientY === 0;
    const hiddenFocus =  !isKeyboardClick;
    this.props.dispatch(ac.OnlyToMain({type: at.HANDOFF_SEARCH_TO_AWESOMEBAR, data: {hiddenFocus}}));
    this.props.dispatch({type: at.FOCUS_SEARCH});

    // TODO: Send a telemetry ping. BUG 1514732
  }

  componentWillUnmount() {
    delete window.gContentSearchController;
  }

  onInputMount(input) {
    if (input) {
      // The "healthReportKey" and needs to be "newtab" or "abouthome" so that
      // BrowserUsageTelemetry.jsm knows to handle events with this name, and
      // can add the appropriate telemetry probes for search. Without the correct
      // name, certain tests like browser_UsageTelemetry_content.js will fail
      // (See github ticket #2348 for more details)
      const healthReportKey = IS_NEWTAB ? "newtab" : "abouthome";

      // The "searchSource" needs to be "newtab" or "homepage" and is sent with
      // the search data and acts as context for the search request (See
      // nsISearchEngine.getSubmission). It is necessary so that search engine
      // plugins can correctly atribute referrals. (See github ticket #3321 for
      // more details)
      const searchSource = IS_NEWTAB ? "newtab" : "homepage";

      // gContentSearchController needs to exist as a global so that tests for
      // the existing about:home can find it; and so it allows these tests to pass.
      // In the future, when activity stream is default about:home, this can be renamed
      window.gContentSearchController = new ContentSearchUIController(input, input.parentNode,
        healthReportKey, searchSource);
      addEventListener("ContentSearchClient", this);
    } else {
      window.gContentSearchController = null;
      removeEventListener("ContentSearchClient", this);
    }
  }

  /*
   * Do not change the ID on the input field, as legacy newtab code
   * specifically looks for the id 'newtab-search-text' on input fields
   * in order to execute searches in various tests
   */
  render() {
    const wrapperClassName = [
      "search-wrapper",
      this.props.hide && "search-hidden",
      this.props.focus && "search-active",
    ].filter(v => v).join(" ");

    return (<div className={wrapperClassName}>
      {this.props.showLogo &&
        <div className="logo-and-wordmark">
          <div className="logo" />
          <div className="wordmark" />
        </div>
      }
      {!this.props.handoffEnabled &&
      <div className="search-inner-wrapper">
        <label htmlFor="newtab-search-text" className="search-label">
          <span className="sr-only"><FormattedMessage id="search_web_placeholder" /></span>
        </label>
        <input
          id="newtab-search-text"
          maxLength="256"
          placeholder={this.props.intl.formatMessage({id: "search_web_placeholder"})}
          ref={this.onInputMount}
          title={this.props.intl.formatMessage({id: "search_web_placeholder"})}
          type="search" />
        <button
          id="searchSubmit"
          className="search-button"
          onClick={this.onSearchClick}
          title={this.props.intl.formatMessage({id: "search_button"})}>
          <span className="sr-only"><FormattedMessage id="search_button" /></span>
        </button>
      </div>
      }
      {this.props.handoffEnabled &&
        <div className="search-inner-wrapper">
          <button
            className="search-handoff-button"
            onClick={this.onSearchHandoffClick}
            title={this.props.intl.formatMessage({id: "search_web_placeholder"})}>
            <div className="fake-textbox">{this.props.intl.formatMessage({id: "search_web_placeholder"})}</div>
            <div className="fake-caret" />
          </button>
          {/*
            This dummy and hidden input below is so we can load ContentSearchUIController.
            Why? It sets --newtab-search-icon for us and it isn't trivial to port over.
          */}
          <input
            type="search"
            style={{display: "none"}}
            ref={this.onInputMount} />
        </div>
      }
    </div>);
  }
}

export const Search = connect()(injectIntl(_Search));
