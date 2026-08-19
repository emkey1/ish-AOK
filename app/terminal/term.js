hterm.defaultStorage = new lib.Storage.Memory();
window.onload = async function() {
    await lib.init();
    window.term = new hterm.Terminal();

    // make everything invisible so as to not be embarrassing
    term.getPrefs().set('background-color', 'transparent');
    term.getPrefs().set('foreground-color', 'transparent');
    term.getPrefs().set('cursor-color', 'transparent');

    term.getPrefs().set('terminal-encoding', 'iso-2022');
    term.getPrefs().set('enable-resize-status', false);
    term.getPrefs().set('copy-on-select', false);
    term.getPrefs().set('enable-clipboard-notice', false);
    term.getPrefs().set('user-css-text', termCss);
    term.getPrefs().set('screen-padding-size', 4);
    // Creating and preloading the <audio> element for this sometimes hangs WebKit on iOS 16 for some reason. Can be most easily reproduced by resetting a simulator and starting the app. System logs show Fig hanging while trying to do work.
    term.getPrefs().set('audible-bell-sound', '');

    term.onTerminalReady = onTerminalReady;
    term.decorate(document.getElementById('terminal'));
};

var termCss = `
x-screen {
    background: transparent !important;
    overflow: hidden !important;
    -webkit-tap-highlight-color: transparent;
}
x-row {
  text-rendering: optimizeLegibility;
  font-variant-ligatures: normal;
}
.uri-node {
  text-decoration: underline;
}
`;

function onTerminalReady() {

// Shorthand for JS -> native IPC
const native = new Proxy({}, {
    get(obj, prop) {
        return (...args) => {
            if (args.length == 0)
                args = null;
            else if (args.length == 1)
                args = args[0];
            webkit.messageHandlers[prop].postMessage(args);
        };
    },
});

// Functions for native -> JS
window.exports = {};

term.io.push();
term.reset();

let oldProps = {};
function syncProp(name, value) {
    if (oldProps[name] !== value)
        native.propUpdate(name, value);
}
// Force a full viewport repaint after output, coalesced to one per animation
// frame. Under iOS WKWebView, hterm's incremental dirty-row redraw can miss
// changed row ranges for rapidly-updated regions (e.g. htop's meters and
// per-process fields), leaving stale pixels until a full repaint (^L). An
// explicit scheduleInvalidate() + scheduleRedraw() repaints the whole visible
// viewport, fixing the partially-painted rows. Mirrors the PSCAL iPad port.
let pendingViewportRefresh = false;
function scheduleViewportRefresh() {
    if (pendingViewportRefresh)
        return;
    pendingViewportRefresh = true;
    requestAnimationFrame(() => {
        pendingViewportRefresh = false;
        if (!term.scrollPort_)
            return;
        term.scrollPort_.scheduleInvalidate();
        term.scrollPort_.scheduleRedraw();
        syncScroll();
    });
}
exports.write = (data) => {
    // Feed raw bytes into hterm's writeUTF8, which uses an internal *streaming*
    // UTF-8 decoder (TextDecoder with {stream: true}). The previous path ran a
    // standalone, non-streaming TextDecoder here and then writeUTF16: any
    // multibyte UTF-8 sequence that straddled the boundary between two coalesced
    // output chunks got flushed as U+FFFD on both sides, corrupting glyphs like
    // htop's box-drawing/meter characters. Streaming decode reassembles them.
    term.io.writeUTF8(lib.codec.stringToCodeUnitArray(data));
    syncProp('applicationCursor', term.keyboard.applicationCursor);
    scheduleViewportRefresh();
};
term.io.sendString = term.io.onVTKeyStroke = (data) => {
    native.sendInput(data);
};

// hterm size updates native size
term.io.onTerminalResize = () => native.resize();
exports.getSize = () => [term.screenSize.width, term.screenSize.height];

// selection, copying
term.scrollPort_.screen_.contentEditable = false;
term.blur();
term.focus();
exports.copy = () => term.copySelectionToClipboard();

// focus
// This listener blocks blur events that come in because the webview has lost first responder
term.scrollPort_.screen_.addEventListener('blur', (e) => {
    if (e.target.ownerDocument.activeElement == e.target) {
        e.stopImmediatePropagation();
    }
}, {capture: true});
term.scrollPort_.screen_.addEventListener('mousedown', (e) => {
    // Taps while there is a selection should be left to the selection view
    if (document.getSelection().rangeCount != 0) return;
    native.focus();
});
exports.setFocused = (focus) => {
    if (focus)
        term.focus();
    else
        term.blur();
};
term.scrollPort_.screen_.addEventListener('focus', (e) => native.syncFocus());

// scrolling
// Disable hterm builtin touch scrolling
term.scrollPort_.onTouch = (e) => {
    // Convince hterm that we called preventDefault() and that it shouldn't do more handling, but don't actually call it because that would break text selection
    Object.defineProperty(e, 'defaultPrevented', {value: true});
};
// Scroll to bottom wrapper
exports.scrollToBottom = () => term.scrollEnd();
// Set scroll position
exports.newScrollTop = (y) => {
    // two lines instead of one because the value you read out of scrollTop can be different from the value you write into it
    term.scrollPort_.screen_.scrollTop = y;
    lastScrollTop = term.scrollPort_.screen_.scrollTop;
};

// Send scroll height and position to native code
let lastScrollHeight, lastScrollTop;
function syncScroll() {
    const scrollHeight = parseFloat(term.scrollPort_.scrollArea_.style.height);
    if (scrollHeight != lastScrollHeight)
        native.newScrollHeight(scrollHeight);
    lastScrollHeight = scrollHeight;

    const scrollTop = term.scrollPort_.screen_.scrollTop;
    if (scrollTop != lastScrollTop)
        native.newScrollTop(scrollTop);
    lastScrollTop = scrollTop;
}

const realSyncScrollHeight = hterm.ScrollPort.prototype.syncScrollHeight;
hterm.ScrollPort.prototype.syncScrollHeight = function() {
    realSyncScrollHeight.call(this);
    syncScroll();
};
term.scrollPort_.screen_.addEventListener('scroll', syncScroll);

exports.updateStyle = ({foregroundColor, backgroundColor, fontFamily, fontSize, lineHeight, colorPaletteOverrides, blinkCursor, cursorShape}) => {
    term.getPrefs().set('background-color', backgroundColor);
    term.getPrefs().set('foreground-color', foregroundColor);
    term.getPrefs().set('cursor-color', foregroundColor);
    term.getPrefs().set('font-family', fontFamily);
    term.getPrefs().set('font-size', fontSize);
    // Cell height as a multiple of the font's measured maximum extent. See the
    // 'line-height' pref: a block glyph is shorter than the box its background
    // fills, and the leftover shows as a band beside a Powerline separator.
    term.getPrefs().set('line-height', lineHeight);
    term.getPrefs().set('color-palette-overrides', colorPaletteOverrides);
    term.getPrefs().set('cursor-blink', blinkCursor);
    term.getPrefs().set('cursor-shape', cursorShape);
};

exports.getCharacterSize = () => {
    return [term.scrollPort_.characterSize.width, term.scrollPort_.characterSize.height];
};

exports.clearScrollback = () => term.clearScrollback();
exports.setUserGesture = () => term.accessibilityReader_.hasUserGesture = true;
// Padding between the terminal text and the webview edge. Native lowers this to 0
// when "Maximize Screen Space" is on with an external keyboard to reclaim edge space.
exports.setScreenPaddingSize = (size) => term.getPrefs().set('screen-padding-size', size);

// Scrollback search. hterm ships a complete incremental-search engine
// (hterm.FindBar), already constructed and decorated for every terminal, but the
// only thing that ever triggers it upstream is a Ctrl+Shift+F keymap entry that
// can never fire here: term.js replaces onVTKeyStroke to forward every keystroke
// to native before hterm's keyboard sees it. So the engine is driven directly
// from the native find bar in TerminalViewController instead.
//
// Deliberately not findBar.display()/findBar.close(): both move focus inside the
// webview (display() focuses hterm's own <input>, close() focuses the
// contenteditable x-screen), which fights TerminalView for first responder and
// can summon the WKWebView's own keyboard on top of ours. The native UITextField
// owns the query text; only the search engine and its highlight overlay are used
// here, and hterm's own find-bar chrome stays hidden the whole time.
const findBar = term.findBar;

exports.findOpen = () => {
    if (findBar.isVisible)
        return;
    findBar.scrollPort_.subscribe('scroll', findBar.onScroll_);
    findBar.resultScreen_.style.display = '';
    // Load-bearing: scheduleNotifyChanges() early-returns unless isVisible, and
    // that is what keeps results live as new output scrolls in underneath.
    findBar.isVisible = true;
    // Resync before the overlay is shown, as upstream display() does. findClose
    // empties the input but leaves the previous highlight rows in the (hidden)
    // result screen, so without this a reopen can flash stale highlights.
    findBar.input_.dispatchEvent(new Event('input'));
};

exports.findSetText = (text) => {
    findBar.input_.value = text;
    // onInput_ reads event.target.value, so this routes into the existing
    // setTimeout-batched syncResults_ rather than searching synchronously --
    // which is what keeps a search over a long scrollback off the main thread.
    findBar.input_.dispatchEvent(new Event('input'));
};

// Both early-return unless the matching arrow carries class 'enabled', which
// findInRow_ adds once there is a result, so these are safe to call blind.
exports.findNext = () => findBar.onNext_();
exports.findPrevious = () => findBar.onPrevious_();

exports.findClose = () => {
    if (!findBar.isVisible)
        return;
    // close() minus its terminal_.focus(); see the note above.
    findBar.input_.value = '';
    findBar.searchText_ = '';
    findBar.resultScreen_.style.display = 'none';
    findBar.scrollPort_.unsubscribe('scroll', findBar.onScroll_);
    findBar.isVisible = false;
    findBar.stopSearch();
    findBar.results_ = {};
    findBar.resultCount_ = 0;
};

// Push the result counter to native instead of having native poll for it. Same
// monkey-patch idiom as syncScrollHeight above. Batched searching means the
// count climbs over several calls on a long scrollback, and this is what lets
// the native label track it live instead of sitting at 0/0.
const realUpdateCounterLabel = hterm.FindBar.prototype.updateCounterLabel_;
hterm.FindBar.prototype.updateCounterLabel_ = function() {
    realUpdateCounterLabel.call(this);
    native.findCount([this.resultCount_, this.selectedOrdinal_]);
};

hterm.openUrl = (url) => native.openLink(url);

native.load();
native.syncFocus();

}
