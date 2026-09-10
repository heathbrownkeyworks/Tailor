/* Tailor consumer of Meridian.Input/1. Skyrim input stays owned by Meridian. */
(function () {
  'use strict';
  let input, stopActions, stopState, observer;
  let opened = false, screen = '', layer = null, disposeScreen, disposeLayer, disposePreview;
  let previewMode = false, numberEditing = null, synchronizing = false;
  const selections = new Map();
  const byId = id => document.getElementById(id);
  const key = () => (_wigCurrentScreen || _currentScreen) + ':' + _activeTab;
  const identity = el => el?.dataset.meridianId || el?.id || '';
  const textField = el => el && (el.tagName === 'TEXTAREA' || el.isContentEditable ||
    (el.tagName === 'INPUT' && !['range', 'checkbox', 'radio', 'button', 'color', 'number'].includes(el.type)));
  const visible = el => el && el.isConnected && el.getClientRects().length &&
    getComputedStyle(el).visibility !== 'hidden' && !el.closest('[hidden],[inert],.panel:not(.active),.rdock:not(.open)');

  function candidates(root) {
    // Keep native click handlers as the action implementation. Rows built by
    // renderers carry stable record IDs; child buttons inherit that identity.
    const counts = new Map();
    return Array.from(root.querySelectorAll('*')).filter(el => {
      const native = el.matches('button,input,select,textarea,a[href]');
      const swatch = el.matches('#wigHairColorGrid > div,.cc-saved-swatch');
      if (!native && !swatch && typeof el.onclick !== 'function' && !el.hasAttribute('tabindex')) return false;
      const disabled = el.matches(':disabled,[aria-disabled=true],.disabled');
      if (!native) {
        const index = disabled ? -1 : 0;
        if (el.tabIndex !== index) el.tabIndex = index;
        if (!el.hasAttribute('role')) el.setAttribute('role', 'button');
      }
      if (!identity(el) || el.dataset.tailorFocusBase) {
        const parent = el.parentElement?.closest('[data-meridian-id],[id]');
        const name = el.getAttribute('onclick') || el.getAttribute('aria-label') || el.title || el.value || el.textContent.trim();
        const base = el.dataset.tailorFocusBase || identity(parent) + ':' + el.tagName + ':' + name;
        el.dataset.tailorFocusBase = base;
        const ordinal = counts.get(base) || 0;
        counts.set(base, ordinal + 1);
        el.dataset.meridianId = base + ':' + ordinal;
      }
      return !disabled && visible(el) && el.tabIndex >= 0;
    });
  }

  function closePopups() {
    closeAllDropdowns(); closePluginDropdown(); closeWigMoveDropdowns();
    hideSetAllDropdown(); hideCycleDropdown(); hideWigCycleDropdown();
    byId('armorModResults').style.display = 'none';
    ['sitCategoryPicker', 'wigSitCategoryPicker'].forEach(id => { if (byId(id)) byId(id).style.display = 'none'; });
  }

  function topLayer() {
    for (const [id, back, initial] of [
      ['exportNameOverlay', closeExportName, 'exportFilename'],
      ['confirmOverlay', closeConfirm, 'controllerConfirmCancel'],
      ['promptOverlay', closePrompt, 'promptInput']
    ]) {
      if (visible(byId(id))) return {id, root:byId(id), back, initial};
    }
    const popup = Array.from(document.querySelectorAll('.t-dropdown-list.open,.wig-move-popover[data-open="1"],#setAllDropdown,#cycleDropdownList,#wigCycleDropdownList,#armorModResults,#wigPluginDropdownList,#sitCategoryPicker,#wigSitCategoryPicker')).find(el =>
      visible(el) && !(textField(document.activeElement) && el.parentElement?.contains(document.activeElement)));
    return popup ? {id:identity(popup) || popup.className, root:popup, back:closePopups} : null;
  }

  function back() {
    switch (_wigCurrentScreen || _currentScreen) {
      case 'cycle': doCycleCancel(); break;
      case 'wigCycle': doWigCycleCancel(); break;
      case 'create': case 'edit': cancelCreateOutfit(); break;
      case 'library': closeLibrary(); break;
      case 'categories': case 'blacklist': showScreen('library'); break;
      case 'wigLibrary': case 'wigSituations': wigShowMain(); break;
      case 'wigAdd': leaveAddWigScreen(); break;
      case 'wigBlacklist': wigShowLibrary(); break;
      case 'wigHairColor': closeHairColor(); break;
      case 'wigCustomColors': closeCustomColors(); break;
      case 'situations': case 'export': case 'import': showScreen('main', 'outfits'); break;
      default: railClose();
    }
    sync();
    return true;
  }

  function exitPreview() {
    if (!previewMode) return;
    previewMode = false;
    stopPreviewRotation();
    disposePreview?.(); disposePreview = null;
    byId('controllerPreview').setAttribute('aria-pressed', 'false');
    prompts();
  }

  function enterPreview() {
    if (!input || !opened || !input.getState().active || !_previewActive || layer) return;
    if (previewMode) { exitPreview(); return; }
    if (input.getState().mode !== 'navigation') { input.setMode('navigation'); return; }
    stopPreviewRotation();
    previewMode = true;
    byId('controllerPreview').setAttribute('aria-pressed', 'true');
    disposePreview = input.attachNavigation({root:byId('controllerPreviewScope'), initialFocus:'controllerPreview',
      getCandidates:() => candidates(byId('controllerPreviewScope')), onBack:() => { exitPreview(); return true; },
      onAction:event => {
        if (event.phase === 'cancel') { exitPreview(); return true; }
        if (event.control === 'rightStick') {
          if (event.phase === 'change' && _previewActive && _previewOpenGeneration && !document.hidden) {
            changePreviewRotation(event.x * event.dt * Math.PI / 2);
            sendPreviewRotation();
          }
          return true; // never also scroll the page
        }
        if (event.action === 'tertiary' && event.phase === 'press') { resetPreviewRotation(); return true; }
        if (event.action === 'accept' && event.phase === 'press') {
          const selected = document.activeElement?.id;
          if (selected === 'previewRotateLeft' || selected === 'previewRotateRight') {
            changePreviewRotation((selected === 'previewRotateLeft' ? -1 : 1) * Math.PI / 12);
            sendPreviewRotation(); return true;
          }
        }
        return ['previousTab', 'nextTab', 'secondary'].includes(event.action);
      }});
    prompts();
  }

  function action(event) {
    if (event.phase === 'cancel') { numberEditing = null; exitPreview(); return false; }
    // Removing scopes on close is synchronous; swallow the rest of a packet
    // before native Unfocus/Hide arrives through the ordinary close callback.
    if (!opened) return true;
    if (event.action === 'accept' && input.getState().mode === 'cursor') return false;
    if (previewMode) return false;
    const selected = document.activeElement;
    if (event.action === 'accept' && event.phase === 'press' &&
        ['previewRotateLeft','previewRotateRight'].includes(selected?.id) && !layer) {
      enterPreview();
      if (previewMode) {
        changePreviewRotation((selected.id === 'previewRotateLeft' ? -1 : 1) * Math.PI / 12);
        sendPreviewRotation();
      }
      return true;
    }
    if (numberEditing && numberEditing !== selected) numberEditing = null;
    if (selected?.matches('input[type=number]')) {
      if (event.action === 'accept' && event.phase === 'press') {
        numberEditing = numberEditing ? null : selected;
        prompts(); return true;
      }
      if (numberEditing && event.action === 'cancel' && event.phase === 'press') {
        numberEditing = null; prompts(); return true;
      }
      if (numberEditing && ['up','down','left','right'].includes(event.action)) {
        if (event.phase === 'press' || event.phase === 'repeat') {
          const sign = ['left','down'].includes(event.action) ? -1 : 1;
          const min = selected.min === '' ? -Infinity : Number(selected.min);
          const max = selected.max === '' ? Infinity : Number(selected.max);
          selected.value = String(Math.min(max, Math.max(min, Number(selected.value) + sign * (Number(selected.step) || 1))));
          selected.dispatchEvent(new Event('input', {bubbles:true}));
          selected.dispatchEvent(new Event('change', {bubbles:true}));
        }
        return true;
      }
    }
    if (layer) return ['previousTab','nextTab','secondary','tertiary'].includes(event.action);
    if (['previousTab','nextTab'].includes(event.action)) {
      if (textField(selected) || numberEditing || selected?.matches('input[type=range],select')) return true;
      if (event.phase !== 'press') return true;
      const next = event.action === 'nextTab';
      if (_currentScreen === 'cycle') (next ? doCycleNext : doCyclePrev)();
      else if (_wigCurrentScreen === 'wigCycle') (next ? doWigCycleNext : doWigCyclePrev)();
      else {
        const rails = Array.from(document.querySelectorAll('.rail-btn[data-page]'));
        const current = Math.max(0, rails.findIndex(el => el.classList.contains('active')));
        rails[(current + (next ? 1 : rails.length - 1)) % rails.length].click();
      }
      sync(); return true;
    }
    if (event.action === 'tertiary' && event.phase === 'press' && !textField(selected) && !numberEditing) {
      enterPreview(); return true;
    }
    if (event.action === 'secondary' && event.phase === 'press' && !textField(selected) && !numberEditing) {
      if (_currentScreen === 'cycle') { doCycleConfirm(); sync(); return true; }
      if (_wigCurrentScreen === 'wigCycle') { doWigCycleConfirm(); sync(); return true; }
    }
    return false;
  }

  function prompts() {
    if (!input) return;
    const state = input.getState();
    const controller = opened && state.enabled && state.connected && state.device === 'gamepad';
    const active = String(!!controller);
    if (document.documentElement.dataset.tailorController !== active) document.documentElement.dataset.tailorController = active;
    const footer = byId('controllerPrompts');
    const parts = [];
    const add = (action, label) => {
      const prompt = input.getPrompt(action);
      if (prompt.label) parts.push(prompt.label + ' ' + label);
    };
    if (previewMode) {
      parts.push('Right stick Rotate'); add('tertiary', 'Front'); add('cancel', 'Return');
    } else {
      add('accept', numberEditing ? 'Done' : 'Select / Edit'); add('cancel', 'Back');
      if (textField(document.activeElement)) parts.push('Keyboard to type');
      else if (numberEditing) parts.push('Directions Adjust');
      else if (!layer) {
        add('previousTab', _currentScreen === 'cycle' || _wigCurrentScreen === 'wigCycle' ? 'Previous' : 'Page');
        add('nextTab', 'Next');
        if (_currentScreen === 'cycle' || _wigCurrentScreen === 'wigCycle') add('secondary', 'Assign');
        if (_previewActive) add('tertiary', 'Rotate');
        add('toggleCursor', state.mode === 'cursor' ? 'Navigation' : 'Cursor');
      }
    }
    const value = parts.join('   ·   ');
    if (footer.textContent !== value) footer.textContent = value;
  }

  function clearScopes() {
    exitPreview(); numberEditing = null;
    disposeLayer?.(); disposeLayer = null; layer = null;
    disposeScreen?.(); disposeScreen = null;
  }

  function sync() {
    if (!input || synchronizing) return;
    synchronizing = true;
    try {
      if (!opened || !input.getState().active || document.hidden) { clearScopes(); prompts(); return; }
      const current = key();
      if (screen !== current || !disposeScreen) {
        if (screen && !layer && !previewMode) selections.set(screen, identity(document.activeElement));
        clearScopes(); screen = current;
        disposeScreen = input.attachNavigation({root:document.body, getCandidates:() => candidates(document.body),
          initialFocus:() => {
            const items = candidates(document.body);
            return items.find(el => identity(el) === selections.get(screen)) ||
              items.find(el => el.closest('.panel.active') && el.matches('.t-cat-row,.t-row')) ||
              items.find(el => el.closest('.panel.active') && !textField(el)) || items[0];
          }, onBack:back});
      }
      if (!_previewActive) exitPreview();
      const nextLayer = topLayer();
      if (layer?.root !== nextLayer?.root) {
        exitPreview(); numberEditing = null;
        disposeLayer?.(); disposeLayer = null; layer = nextLayer;
        if (layer) disposeLayer = input.attachNavigation({root:layer.root, initialFocus:layer.initial,
          getCandidates:() => candidates(layer.root), onBack:() => { layer.back(); sync(); return true; }});
      }
      prompts();
    } finally { synchronizing = false; }
  }

  function init() {
    if (input || window.MeridianInput?.version !== 1) return;
    input = window.MeridianInput;
    stopActions = input.onAction(action);
    stopState = input.onStateChange(state => {
      if (!state.active || !state.connected || state.device !== 'gamepad' || state.mode !== 'navigation') exitPreview();
      sync();
    });
    observer = new MutationObserver(sync);
    observer.observe(document.body, {subtree:true, childList:true, attributes:true,
      attributeFilter:['class','style','disabled','hidden','aria-disabled']});
    sync();
  }

  window.TailorController = {
    init, sync, preview:enterPreview,
    open() { opened = true; init(); sync(); },
    close() {
      if (screen && !layer && !previewMode) selections.set(screen, identity(document.activeElement));
      opened = false; clearScopes(); prompts();
    }
  };
  document.addEventListener('click', sync);
  document.addEventListener('focusin', prompts);
  document.addEventListener('focusout', () => queueMicrotask(sync));
  document.addEventListener('visibilitychange', sync);
  window.addEventListener('blur', exitPreview);
  window.addEventListener('pagehide', () => { window.TailorController.close(); observer?.disconnect(); stopActions?.(); stopState?.(); });
  document.addEventListener('DOMContentLoaded', init);
})();
