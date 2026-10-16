// SPDX-License-Identifier: GPL-3.0-or-later
'use strict';
const $ = id => document.getElementById(id);
let config, activeStore, entries = [], groups = [], selection, stopped = false;
let bootConfig;
let metadataWatcher = false;
let localEntries = [], installedIds = new Set();
let historySnapshot = '';
let settingsBaseline = null;
const observedCompletions = new Set();

try {
    bootConfig = JSON.parse($('bootstrapSettings').textContent);
} catch (_) {}

history.replaceState({
    screen: 'stores',
    depth: 0
}, '', '#stores');

const kindName = type => type === 'installed' ? 'Local library' : type === 'ownfoil' ? 'Ownfoil API' : 'Tinfoil JSON';

function message(value = '', error = false) {
    $('message').textContent = value;
    $('message').className = error ? 'error' : '';
}

function show(id, push = true) {
    if (push && history.state && history.state.screen !== id)
        history.pushState({
            screen: id,
            storeId: activeStore && activeStore.id,
            depth: history.state.depth + 1
        }, '', '#' + id);
    ['stores', 'catalogue', 'settings', 'downloads', 'stopped'].forEach(x => $(x).hidden = x !== id);
    $('backButton').hidden = !history.state || history.state.depth === 0;
    applyAppearance(id === 'settings');
    updateSettingsPrompt();
}

function applyAppearance(preview = false) {
    document.documentElement.dataset.theme = preview ? $('theme').value : (config && config.theme || 'red');
    document.documentElement.dataset.appearance = preview ? ($('lightMode').checked ? 'light' : 'dark') : (config && config.appearance || 'dark');
}

function element(tag, text, className) {
    const e = document.createElement(tag);
    if (text)
        e.textContent = text;
    if (className)
        e.className = className;
    return e;
}

function makeDialog(id, initialFocus) {
    const overlay = $(id);
    let returnFocus;
    function close() {
        overlay.hidden = true;
        document.body.classList.remove('modalOpen');
        if (returnFocus && returnFocus.isConnected)
            returnFocus.focus();
    }
    overlay.addEventListener('click', event => {
        if (event.target === overlay)
            close();
    });
    overlay.addEventListener('keydown', event => {
        if (event.key === 'Escape') {
            event.preventDefault();
            close();
        }
        if (event.key === 'Tab') {
            const controls = [...overlay.querySelectorAll('input,select,button,[tabindex="0"]')].filter(control => !control.disabled && control.getClientRects().length);
            const first = controls[0],
            last = controls[controls.length - 1];
            if (event.shiftKey && document.activeElement === first) {
                event.preventDefault();
                last.focus();
            } else if (!event.shiftKey && document.activeElement === last) {
                event.preventDefault();
                first.focus();
            }
        }
    });
    return {
        open(trigger) {
            returnFocus = trigger || document.activeElement;
            overlay.hidden = false;
            document.body.classList.add('modalOpen');
            $(initialFocus).focus();
        },
        close
    };
}

function assetIcon(name, fallback, className = '') {
    const url = window.PSFOIL_ICONS && window.PSFOIL_ICONS[name];
    if (!url)
        return fallback;
    const img = element('img', '', 'uiIcon ' + className);
    img.src = url;
    img.alt = '';
    img.setAttribute('aria-hidden', 'true');
    return img;
}

function storeSymbol() {
    const ns = 'http://www.w3.org/2000/svg',
    svg = document.createElementNS(ns, 'svg');
    svg.setAttribute('viewBox', '0 0 32 32');
    svg.setAttribute('aria-hidden', 'true');
    svg.classList.add('storeIcon');
    const path = document.createElementNS(ns, 'path');
    path.setAttribute('d', 'M5 13v15h22V13M3 13l3-9h20l3 9M3 13c0 5 6 5 6 0 0 5 7 5 7 0 0 5 7 5 7 0 0 5 6 5 6 0M12 28V19h8v9');
    path.setAttribute('fill', 'none');
    path.setAttribute('stroke', 'currentColor');
    path.setAttribute('stroke-width', '2');
    path.setAttribute('stroke-linejoin', 'round');
    svg.append(path);
    return assetIcon('store', svg, 'storeIcon');
}

function downloadSymbol() {
    const ns = 'http://www.w3.org/2000/svg',
    svg = document.createElementNS(ns, 'svg'),
    path = document.createElementNS(ns, 'path');
    svg.setAttribute('viewBox', '0 0 24 24');
    svg.setAttribute('aria-hidden', 'true');
    path.setAttribute('d', 'M12 3v12m-5-5 5 5 5-5M4 16v5h16v-5');
    path.setAttribute('fill', 'none');
    path.setAttribute('stroke', 'currentColor');
    path.setAttribute('stroke-width', '2');
    path.setAttribute('stroke-linecap', 'round');
    path.setAttribute('stroke-linejoin', 'round');
    svg.append(path);
    return svg;
}

function groupSummary(group) {
    const sorted = group.entries.slice().sort((a, b) => Number(b.version || 0) - Number(a.version || 0));
    const item = sorted.find(e => e.kind === 'UPDATE') || sorted.find(e => e.kind === 'BASE') || sorted[0];
    let version = item && (item.displayVersion || (item.version != null ? 'v' + item.version : 'Version unknown'));
    const count = new Set(group.entries.filter(e => e.kind === 'DLC').map(e => e.appId)).size;
    return version + (count ? ' · ' + count + ' DLC' : '');
}

function symbol(name) {
    const paths = {
        downloads: 'M4 4h16v16H4zM12 7v9m-4-4 4 4 4-4',
        settings: 'M4 6h16M4 12h16M4 18h16M8 3v6m8 0v6m-6 0v6',
        quit: 'M12 2v10M6 5a9 9 0 1 0 12 0',
        add: 'M12 4v16M4 12h16',
        library: 'M3 3h7v7H3zM14 3h7v7h-7zM3 14h7v7H3zM14 17l3 3 5-6',
        tick: 'M4 12l5 5L20 6',
        trash: 'M4 6h16M9 6V3h6v3M6 6l1 15h10l1-15M10 10v7m4-7v7'
    };
    const svg = downloadSymbol();
    svg.querySelector('path').setAttribute('d', paths[name]);
    return assetIcon(name, svg);
}

function iconButton(id, name, label) {
    const button = $(id);
    button.replaceChildren(symbol(name));
    button.classList.add('iconButton');
    button.setAttribute('aria-label', label);
    button.title = label;
}

[['downloadsButton', 'downloads', 'Downloads'], ['settingsButton', 'settings', 'Settings'], ['quitButton', 'quit', 'Quit'], ['addStore', 'add', 'Add store'], ['newStore', 'add', 'Add store']].forEach(args => iconButton(...args));

const brandIcon = assetIcon('logo', null, 'brandIcon');

if (brandIcon)
    $('home').replaceChildren(brandIcon, element('strong', 'PSFoil'));

function updateInstalled(items) {
    localEntries = items;
    installedIds = new Set(items.filter(e => e.kind === 'BASE').map(e => e.titleId));
}

function totalSize(group) {
    const files = new Map();
    group.entries.forEach(e => files.set(e.fileId || e.entryId || e.path || e.appId, e.size));
    const known = [...files.values()].filter(n => n != null && Number.isFinite(Number(n)) && Number(n) >= 0);
    return sizeLabel(known.reduce((sum, n) => sum + Number(n), 0)) + (known.length < files.size ? ' + unknown' : '');
}

async function api(path, options = {}) {
    const headers = Object.assign({
        'Content-Type': 'application/json'
    }, options.headers);
    const response = await fetch('/api/' + path, Object.assign({}, options, {
                headers
            }));
    const body = await response.json();
    if (!response.ok)
        throw new Error(body.error || 'Request failed');
    return body;
}

async function task(button, action) {
    button.disabled = true;
    button.setAttribute('aria-busy', 'true');
    message();
    try {
        await action();
    } catch (e) {
        message(e.message, true);
    } finally {
        button.disabled = false;
        button.removeAttribute('aria-busy');
    }
}

function artwork(entry) {
    const art = element('div', '?', 'art');
    const candidates = [...new Set([entry.artworkUrl, entry.metadata && entry.metadata.iconUrl, entry.metadata && entry.metadata.sourceIconUrl].filter(Boolean))];
    const load = () => {
        const url = candidates.shift();
        if (!url) {
            art.textContent = '?';
            return;
        }
        try {
            const parsed = new URL(url, location.href);
            if ((parsed.origin === location.origin || parsed.protocol === 'https:') && !parsed.username && !parsed.password) {
                const img = document.createElement('img');
                img.src = url;
                img.alt = '';
                img.loading = 'lazy';
                img.referrerPolicy = 'no-referrer';
                img.onerror = load;
                art.textContent = '';
                art.append(img);
            } else
                load();
        } catch (_) {
            load();
        }
    };
    load();
    return art;
}

async function home(push = true) {
    config = bootConfig || await api('settings');
    bootConfig = undefined;
    $('lanAddress').textContent = 'http://' + location.host;
    $('storeGrid').textContent = '';
    const library = element('button', '', 'store localLibrary');
    library.type = 'button';
    library.append(symbol('library'), element('strong', 'Installed games'), element('span', 'On this console', 'badge'));
    library.onclick = () => task(library, () => openStore({
            id: 'installed',
            name: 'Installed games',
            type: 'installed'
        }));
    $('storeGrid').append(library);
    config.stores.forEach(store => {
        const button = element('button', '', 'store');
        button.append(storeSymbol(), element('strong', store.name), element('span', kindName(store.type), 'badge'));
        button.type = 'button';
        button.onclick = () => task(button, () => openStore(store));
        $('storeGrid').append(button);
    });
    if (!config.stores.length)
        $('storeGrid').append(element('p', 'Add your first store in Settings.', 'muted'));
    show('stores', push);
    api('status').then(status => {
        if (status.lanUrls && status.lanUrls.length)
            $('lanAddress').textContent = status.lanUrls.join(' · ');
    }).catch(() => {});
}

async function ensureLibraryMetadata(refreshOnReady = false) {
    if (metadataWatcher)
        return;
    metadataWatcher = true;
    try {
        let status = await api('status');
        if (status.metadata.loaded && !refreshOnReady)
            return;
        if (!status.metadata.loaded)
            message('Library ready. Loading title metadata in the background…');
        while (status.metadata.loading && !stopped) {
            await new Promise(resolve => setTimeout(resolve, 1000));
            status = await api('status');
        }
        if (stopped)
            return;
        if (!status.metadata.loaded)
            await api('metadata/refresh', {
                method: 'POST',
                body: '{}'
            });
        if (activeStore && activeStore.type === 'installed' && !$('catalogue').hidden) {
            const selected = selection;
            await openStore(activeStore, false);
            const group = groups.find(g => g.id === selected);
            if (group)
                selectGame(group);
        }
    } catch (error) {
        if (activeStore && activeStore.type === 'installed')
            message('Library ready. Title metadata unavailable: ' + error.message, true);
    } finally {
        metadataWatcher = false;
    }
}

async function openStore(store, push = true) {
    message('Loading catalogue…');
    let libraryError = '';
    const catalogue = await api(store.type === 'installed' ? 'library' : 'stores/' + encodeURIComponent(store.id) + '/catalogue');
    config = await api('settings');
    if (store.type === 'installed')
        updateInstalled(catalogue.items);
    else {
        try {
            const library = await api('library');
            updateInstalled(library.items);
            if (library.warnings.length)
                libraryError = library.warnings.join('\n');
        } catch (e) {
            updateInstalled([]);
            libraryError = 'Could not read installed games: ' + e.message;
        }
    }
    activeStore = store;
    entries = catalogue.items;
    selection = undefined;
    const map = new Map();
    entries.forEach((entry, i) => {
        const id = entry.titleId || 'unknown-' + i;
        if (!map.has(id))
            map.set(id, {
                id,
                name: entry.name,
                entries: [],
                metadata: entry.metadata,
                artworkUrl: entry.artworkUrl
            });
        const group = map.get(id);
        group.entries.push(entry);
        if (entry.artworkUrl)
            group.artworkUrl = entry.artworkUrl;
        if (entry.kind === 'BASE')
            group.name = entry.name;
    });
    groups = [...map.values()].sort((a, b) => a.name.localeCompare(b.name));
    $('storeType').textContent = kindName(store.type);
    $('storeName').textContent = store.name;
    $('search').value = '';
    $('gameDetails').replaceChildren(element('h2', 'Select a game'));
    $('reload').textContent = store.type === 'installed' ? 'Refresh installed games' : 'Refresh catalogue';
    $('installedFilter').hidden = store.type === 'installed';
    $('showInstalled').checked = config.showInstalled === true;
    $('convertAll').hidden = store.type !== 'installed';
    $('uploadButton').hidden = store.type !== 'installed';
    const compressedGames = groups.filter(g => g.entries.some(e => e.format === 'NSZ')).length;
    $('convertAll').textContent = 'Convert all NSZ (' + compressedGames + ' ' + (compressedGames === 1 ? 'game' : 'games') + ')';
    $('convertAll').disabled = !compressedGames;
    renderGames();
    show('catalogue', push);
    sizeDetailsPanel();
    const warning = libraryError || (catalogue.warnings && catalogue.warnings.length ? catalogue.warnings.join('\n') : '');
    message(warning, Boolean(warning));
    if (store.type === 'installed')
        ensureLibraryMetadata(catalogue.metadataReady === false);
}

function renderGames() {
    $('clearSearch').hidden = !$('search').value;
    const needle = $('search').value.toLowerCase();
    const visible = groups.filter(g => (activeStore.type === 'installed' || $('showInstalled').checked || !installedIds.has(g.id)) && (g.name.toLowerCase().includes(needle) || g.id.toLowerCase().includes(needle)));
    $('games').className = 'games ' + $('view').value;
    $('games').textContent = '';
    $('count').textContent = visible.length + (visible.length === 1 ? ' title · ' : ' titles · ') + entries.length + ' content entries';
    // Render in batches so a large catalogue does not stall the console browser.
    const generation = (renderGames.generation || 0) + 1;
    renderGames.generation = generation;
    let offset = 0;
    function batch() {
        if (renderGames.generation !== generation)
            return;
        visible.slice(offset, offset + 100).forEach(group => {
            const b = element('button', '', 'game' + ($('view').value === 'list' ? ' game-row' : '') + (selection === group.id ? ' selected' : ''));
            b.type = 'button';
            b.setAttribute('aria-pressed', String(selection === group.id));
            b.dataset.group = group.id;
            b.append(artwork(group));
            if (activeStore.type !== 'installed' && installedIds.has(group.id)) {
                const tick = element('span', '', 'installedTick');
                tick.append(symbol('tick'));
                tick.setAttribute('aria-label', 'Installed locally');
                tick.title = 'Installed locally';
                b.append(tick);
            }
            const text = element('span', '', 'gameText');
            text.append(element('strong', group.name));
            if (activeStore.type === 'installed' && group.entries.some(e => e.format === 'NSZ'))
                text.append(element('span', 'NSZ · conversion required', 'nszBadge'));
            const info = element('small', groupSummary(group), 'gameInfo');
            if ($('view').value === 'list')
                info.append(element('span', (group.id.startsWith('unknown-') ? 'Title ID unknown' : group.id), 'gameMeta'), element('span', totalSize(group), 'gameMeta'));
            b.append(text, info);
            b.onclick = () => selectGame(group);
            $('games').append(b);
        });
        offset += 100;
        if (offset < visible.length)
            requestAnimationFrame(batch);
    }
    batch();
    if (!visible.length)
        $('games').append(element('p', 'No matching games.', 'muted'));
}

function sizeLabel(value) {
    if (value == null || !Number.isFinite(Number(value)) || Number(value) < 0)
        return 'size unknown';
    const bytes = Number(value);
    if (bytes < 1000000)
        return (bytes / 1000).toLocaleString('en-US', {
            maximumFractionDigits: 1
        }) + ' KB';
    return bytes < 1000000000 ? Math.round(bytes / 1000000).toLocaleString('en-US') + ' MB' : (bytes / 1000000000).toFixed(2) + ' GB';
}

function selectGame(group) {
    selection = group.id;
    document.querySelectorAll('.game').forEach(b => {
        const selected = b.dataset.group === selection;
        b.classList.toggle('selected', selected);
        b.setAttribute('aria-pressed', String(selected));
    });
    const panel = $('gameDetails');
    panel.scrollTop = 0;
    panel.textContent = '';
    panel.append(artwork(group), element('h2', group.name));
    if (!group.id.startsWith('unknown-'))
        panel.append(element('p', group.id, 'muted'));
    const updates = group.entries.filter(e => e.kind === 'UPDATE');
    const dlc = group.entries.filter(e => e.kind === 'DLC');
    if (activeStore.type === 'installed')
        panel.append(element('h3', 'Installed content'));
    if (activeStore.type === 'ownfoil' || activeStore.type === 'installed') {
        const latest = updates.slice().sort((a, b) => Number(b.version) - Number(a.version))[0];
        const base = group.entries.filter(e => e.kind === 'BASE').sort((a, b) => Number(b.version || 0) - Number(a.version || 0))[0];
        panel.append(element('p', latest ? 'Latest update: ' + (latest.displayVersion || 'v' + latest.version) : base ? 'Base version: ' + (base.displayVersion || (base.version == null ? 'unknown' : 'v' + base.version)) : 'Base version unknown'));
        if (activeStore.type !== 'installed')
            panel.append(downloadAllButton(group));
        else {
            if (group.entries.some(e => e.format === 'NSZ')) {
                const convert = element('button', 'Convert NSZ to NSP', 'downloadAll');
                convert.type = 'button';
                convert.onclick = () => task(convert, () => convertInstalled(group.entries));
                panel.append(convert);
            }
            const remove = element('button', '', 'downloadAll deleteAll');
            remove.type = 'button';
            remove.append(symbol('trash'), element('span', 'Delete all'));
            remove.onclick = () => confirmDeletion(group, group.entries);
            panel.append(remove);
        }
        panel.append(element('p', new Set(dlc.map(x => x.appId)).size + ' DLC items listed'));
    } else {
        panel.append(element('p', 'Version: ' + groupSummary(group)), downloadAllButton(group), element('p', 'Catalogue versions and content types are filename hints. The downloaded package is inspected before sorting.'));
    }
    const list = element('ul');
    group.entries.forEach(e => {
        const row = element('li', '', 'contentRow'),
        label = element('span', (e.kind === 'UNKNOWN' ? e.name : e.kind + ' · ' + (e.displayVersion || (e.version == null ? 'version unknown' : 'v' + e.version))) + ' · ' + sizeLabel(e.size));
        const button = element('button', '', 'contentDownload');
        button.append(assetIcon('download', downloadSymbol()));
        button.type = 'button';
        button.disabled = !e.entryId;
        button.setAttribute('aria-label', 'Download ' + (e.kind === 'UNKNOWN' ? e.name : e.kind) + ' · ' + group.name);
        button.title = 'Download ' + e.kind;
        button.onclick = () => task(button, async() => {
            await queueDownload(e);
            message('Added to downloads.');
        });
        row.append(label);
        if (activeStore.type === 'installed') {
            const remove = element('button', '', 'contentDownload remove');
            remove.type = 'button';
            remove.append(symbol('trash'));
            remove.disabled = !e.fileId;
            remove.setAttribute('aria-label', 'Delete ' + e.kind + ' · ' + group.name);
            remove.title = 'Delete ' + e.kind;
            remove.onclick = () => confirmDeletion(group, [e]);
            row.append(remove, element('p', e.path, 'muted'));
        } else
            row.append(button);
        list.append(row);
    });
    if (activeStore.type === 'installed')
        panel.append(list);
    else {
        const individual = element('details', '', 'individualDownloads');
        individual.append(element('summary', 'Individual files (' + group.entries.length + ')'), list);
        panel.append(individual);
    }
    if (group.metadata && group.metadata.description)
        panel.append(element('p', group.metadata.description));
    sizeDetailsPanel();
}

function downloadAllButton(group) {
    const all = element('button', '', 'downloadAll');
    all.append(assetIcon('download', downloadSymbol()), element('span', 'Download all'));
    all.type = 'button';
    all.onclick = () => task(all, async() => {
        const latest = new Map();
        group.entries.forEach(e => {
            const key = e.appId || e.entryId;
            const old = latest.get(key);
            if (!old || Number(e.version || 0) > Number(old.version || 0))
                latest.set(key, e);
        });
        const entryIds = [...new Set([...latest.values()].map(item => item.entryId).filter(Boolean))];
        await api('downloads', {
            method: 'POST',
            body: JSON.stringify({
                storeId: activeStore.id,
                entryIds,
                title: group.name
            })
        });
        message('Game and add-ons added to downloads.');
        await refreshDownloads();
    });
    return all;
}

async function convertInstalled(items) {
    const fileIds = [...new Set(items.filter(e => e.format === 'NSZ').map(e => e.fileId))];
    const result = await api('library/convert', {
        method: 'POST',
        body: JSON.stringify({
            fileIds
        })
    });
    message(result.games + ' ' + (result.games === 1 ? 'game' : 'games') + ' queued for conversion. Original NSZ files are removed after verification.');
    await refreshDownloads();
}

$('convertAll').onclick = () => task($('convertAll'), () => convertInstalled(entries));

function confirmDeletion(group, items) {
    const files = [...new Map(items.filter(e => e.fileId).map(e => [e.fileId, e])).values()];
    if (!files.length)
        return;
    const panel = $('gameDetails');
    panel.replaceChildren(element('h2', 'Delete installed files?'), element('p', 'These files will be permanently deleted. Any other content bundled inside them will also be removed.'));
    const list = element('ul');
    files.forEach(e => list.append(element('li', e.path + ' · ' + sizeLabel(e.size))));
    panel.append(list);
    const remove = element('button', 'Delete permanently', 'remove'),
    cancel = element('button', 'Cancel', 'secondary');
    remove.type = cancel.type = 'button';
    remove.onclick = () => task(remove, async() => {
        const result = await api('library/delete', {
            method: 'POST',
            body: JSON.stringify({
                fileIds: files.map(e => e.fileId),
                confirmed: true
            })
        });
        await openStore(activeStore, false);
        message(result.errors.length ? result.errors.map(e => e.path + ': ' + e.error).join('\n') : result.deleted.length + ' file(s) deleted.', Boolean(result.errors.length));
    });
    cancel.onclick = () => selectGame(group);
    panel.append(remove, cancel);
    panel.scrollTop = 0;
    sizeDetailsPanel();
}

function sizeDetailsPanel() {
    if ($('catalogue').hidden)
        return;
    const panel = $('gameDetails'),
    top = Math.max(18, panel.getBoundingClientRect().top);
    panel.style.maxHeight = Math.max(180, window.innerHeight - top - 18) + 'px';
}

window.addEventListener('resize', sizeDetailsPanel);
window.addEventListener('scroll', sizeDetailsPanel, {
    passive: true
});

async function queueDownload(entry) {
    return api('downloads', {
        method: 'POST',
        body: JSON.stringify({
            storeId: activeStore.id,
            entryId: entry.entryId
        })
    });
}

function storeEditor(store = {}) {
    const box = element('div', '', 'storeEditor');
    box.dataset.id = store.id || '';
    const header = element('div', '', 'storeEditorHeader'),
    title = element('strong', store.name || 'New store');
    const edit = element('button', 'Edit', 'secondary'),
    remove = element('button', 'Remove', 'remove');
    edit.type = remove.type = 'button';
    const fields = element('div', '', 'fields');
    fields.hidden = Boolean(store.id);
    box.classList.toggle('editing', !fields.hidden);
    edit.setAttribute('aria-expanded', String(!fields.hidden));
    edit.onclick = () => {
        fields.hidden = !fields.hidden;
        box.classList.toggle('editing', !fields.hidden);
        edit.textContent = fields.hidden ? 'Edit' : 'Collapse';
        edit.setAttribute('aria-expanded', String(!fields.hidden));
    };
    remove.onclick = () => {
        box.remove();
        updateSettingsPrompt();
    };
    header.append(title, edit, remove);
    const add = (name, label, type, value) => {
        const l = element('label', label);
        const input = element('input');
        input.name = name;
        input.type = type;
        input.value = value || '';
        input.required = name === 'name' || name === 'url';
        if (type === 'password') {
            input.autocomplete = 'new-password';
            input.placeholder = store.hasPassword ? 'Leave blank to retain saved password' : 'Store password';
        }
        l.append(input);
        fields.append(l);
        if (name === 'name')
            input.oninput = () => {
                title.textContent = input.value || 'New store';
            };
    };
    add('name', 'Store name', 'text', store.name);
    const label = element('label', 'Store type');
    const type = element('select');
    type.name = 'type';
    ['ownfoil', 'tinfoil'].forEach(value => {
        const option = element('option', kindName(value));
        option.value = value;
        type.append(option);
    });
    type.value = store.type || 'ownfoil';
    label.append(type);
    fields.append(label);
    add('url', 'Store URL', 'text', store.url || 'http://10.0.0.1:8465');
    add('username', 'Store username', 'text', store.username);
    add('password', 'Store password', 'password', '');
    edit.textContent = fields.hidden ? 'Edit' : 'Collapse';
    box.append(header, fields);
    $('storeFields').append(box);
}

function readStoreEditor(box) {
    const store = {
        id: box.dataset.id
    };
    box.querySelectorAll('input,select').forEach(input => {
        if (input.name !== 'password' || input.value)
            store[input.name] = input.value;
    });
    return store;
}

function settingsDraft() {
    return {
        region: $('region').value,
        language: $('language').value,
        theme: $('theme').value,
        appearance: $('lightMode').checked ? 'light' : 'dark',
        downloadDirectory: $('downloadDirectory').value,
        splitDownloads: $('splitDownloads').checked,
        stores: [...$('storeFields').querySelectorAll('.storeEditor')].map(readStoreEditor)
    };
}

function settingsChanged() {
    return settingsBaseline !== null && JSON.stringify(settingsDraft()) !== settingsBaseline;
}

function updateSettingsPrompt() {
    const visible = !$('settings').hidden && settingsChanged();
    $('settingsSavePrompt').hidden = !visible;
    document.body.classList.toggle('settingsDirty', visible);
}

async function loadRawSettings() {
    $('rawConfigJson').textContent = 'Loading saved configuration…';
    try {
        const result = await api('settings/raw');
        $('rawConfigPath').textContent = result.path;
        $('rawConfigJson').textContent = JSON.stringify(result.config, null, 2);
        $('rawConfigNote').textContent = (result.exists ? 'Saved configuration; unsaved edits are not shown.' : 'No settings file exists yet; showing active defaults.') + ' Password values are redacted.';
    } catch (error) {
        $('rawConfigJson').textContent = 'Unable to read saved configuration: ' + error.message;
    }
}

async function settingsPage(push = true, force = false) {
    if (!force && settingsChanged()) {
        show('settings', push);
        return;
    }
    config = await api('settings');
    $('region').value = config.region;
    $('language').value = config.language;
    $('theme').value = config.theme || 'red';
    $('lightMode').checked = config.appearance === 'light';
    $('downloadDirectory').value = config.downloadDirectory || '';
    $('splitDownloads').checked = config.splitDownloads !== false;
    $('storeFields').textContent = '';
    config.stores.forEach(s => storeEditor(s));
    settingsBaseline = JSON.stringify(settingsDraft());
    show('settings', push);
    const status = await api('status');
    $('tileStatus').textContent = status.tile;
    $('rawConfigPath').textContent = status.settingsPath;
    if ($('rawConfigDetails').open)
        await loadRawSettings();
}

$('home').onclick = e => {
    e.preventDefault();
    task($('home'), () => home());
};

function back() {
    if (history.state && history.state.depth > 0)
        history.back();
    else
        task($('backButton'), () => home(false));
}

$('backButton').onclick = back;

window.addEventListener('popstate', () => task($('backButton'), async() => {
        if (stopped)
            return;
        const route = history.state || {
            screen: 'stores',
            depth: 0
        };
        if (route.screen === 'settings')
            await settingsPage(false);
        else if (route.screen === 'catalogue' && activeStore && activeStore.id === route.storeId)
            show('catalogue', false);
        else if (route.screen === 'catalogue') {
            if (route.storeId === 'installed') {
                await openStore({
                    id: 'installed',
                    name: 'Installed games',
                    type: 'installed'
                }, false);
                return;
            }
            config = await api('settings');
            const store = config.stores.find(s => s.id === route.storeId);
            if (store)
                await openStore(store, false);
            else
                await home(false);
        } else if (route.screen === 'downloads') {
            show('downloads', false);
            await refreshDownloads();
        } else
            await home(false);
    })
);

$('settingsButton').onclick = () => task($('settingsButton'), () => settingsPage());

const storeDialog = makeDialog('storeOverlay', 'newStoreName');

function closeStoreOverlay() {
    storeDialog.close();
}

function openStoreOverlay(button) {
    $('addStoreForm').reset();
    $('storeOverlayError').textContent = '';
    storeDialog.open(button);
}

$('addStore').onclick = () => openStoreOverlay($('addStore'));
$('newStore').onclick = () => openStoreOverlay($('newStore'));
$('cancelAddStore').onclick = closeStoreOverlay;
$('addStoreForm').onsubmit = async e => {
    e.preventDefault();
    const save = $('saveNewStore');
    save.disabled = true;
    $('storeOverlayError').textContent = '';
    try {
        const settings = await api('settings');
        const store = {
            name: $('newStoreName').value.trim(),
            type: $('newStoreType').value,
            url: $('storeUrl').value.trim(),
            username: $('storeUsername').value,
            password: $('storePassword').value
        };
        settings.stores.push(store);
        await api('settings', {
            method: 'POST',
            body: JSON.stringify(settings)
        });
        config = await api('settings');
        if (settingsBaseline !== null) {
            const baseline = JSON.parse(settingsBaseline);
            storeEditor(config.stores[config.stores.length - 1]);
            baseline.stores.push(readStoreEditor($('storeFields').lastElementChild));
            settingsBaseline = JSON.stringify(baseline);
            updateSettingsPrompt();
        }
        if ($('settings').hidden)
            await home(false);
        if ($('rawConfigDetails').open)
            await loadRawSettings();
        closeStoreOverlay();
        message('Store saved.');
    } catch (error) {
        $('storeOverlayError').textContent = error.message;
    } finally {
        save.disabled = false;
    }
};
$('cancelSettings').onclick = back;
$('search').oninput = renderGames;
$('view').onchange = renderGames;
$('clearSearch').onclick = () => {
    $('search').value = '';
    renderGames();
    $('search').focus();
};
$('showInstalled').onchange = () => task($('showInstalled'), async() => {
    const showInstalled = $('showInstalled').checked;
    try {
        await api('preferences', {
            method: 'POST',
            body: JSON.stringify({
                showInstalled
            })
        });
        config.showInstalled = showInstalled;
        renderGames();
    } catch (e) {
        $('showInstalled').checked = !showInstalled;
        throw e;
    }
});
$('reload').onclick = () => task($('reload'), () => openStore(activeStore));
$('settingsForm').onsubmit = e => e.preventDefault();
$('settingsForm').addEventListener('input', updateSettingsPrompt);
$('settingsForm').addEventListener('change', updateSettingsPrompt);
$('rawConfigDetails').ontoggle = () => {
    if ($('rawConfigDetails').open)
        loadRawSettings();
};
$('discardSettings').onclick = () => task($('discardSettings'), () => settingsPage(false, true));
$('settingsForm').addEventListener('keydown', e => {
    if (e.key === 'Enter' && e.target.tagName === 'INPUT')
        e.preventDefault();
});
$('theme').onchange = $('lightMode').onchange = () => applyAppearance(true);
$('saveSettings').onclick = () => {
    document.querySelectorAll('.storeEditor input:invalid').forEach(input => {
        input.closest('.fields').hidden = false;
        input.closest('.storeEditor').classList.add('editing');
    });
    if (!$('settingsForm').reportValidity())
        return;
    task($('saveSettings'), async() => {
        await api('settings', {
            method: 'POST',
            body: JSON.stringify(settingsDraft())
        });
        // Stay on the form after an explicit save, retaining the saved server IDs.
        await settingsPage(false, true);
        message('Settings saved.');
    });
};
$('repairTile').onclick = () => task($('repairTile'), async() => {
    const result = await api('tile/install', {
        method: 'POST',
        body: '{}'
    });
    $('tileStatus').textContent = result.tile;
    message(result.tile);
});
$('metadataRefresh').onclick = () => task($('metadataRefresh'), async() => {
    message('Downloading titledb metadata…');
    const result = await api('metadata/refresh', {
        method: 'POST',
        body: '{}'
    });
    message('Cached metadata for ' + result.titles + ' titles. Reopen a store to apply it.');
});

async function refreshDownloads() {
    const {
        jobs
    } = await api('downloads');
    const completed = jobs.filter(job => job.status === 'completed' && !observedCompletions.has(job.id));
    if (completed.length) {
        try {
            updateInstalled((await api('library')).items);
            if (activeStore && !$('catalogue').hidden) {
                if (activeStore.type === 'installed') {
                    const selected = selection;
                    await openStore(activeStore, false);
                    const group = groups.find(g => g.id === selected);
                    if (group)
                        selectGame(group);
                } else
                    renderGames();
            }
        } catch (_) {}
        completed.forEach(job => observedCompletions.add(job.id));
    }
    const active = jobs.filter(job => ['queued', 'downloading', 'converting', 'inspecting'].includes(job.status));
    const current = active.find(job => job.status !== 'queued') || active[0];
    $('downloadActivity').hidden = !current;
    sizeDetailsPanel();
    if (current) {
        $('downloadActivityText').textContent = current.name + ' · ' + current.status + (current.status === 'downloading' && current.total ? ' · ' + Math.min(100, Math.floor(current.received / current.total * 100)) + '%' : '') + (active.length > 1 ? ' · ' + (active.length - 1) + ' queued' : '');
        setProgress($('downloadProgress'), current);
    }
    if ($('downloads').hidden)
        return;
    const snapshot = JSON.stringify(jobs);
    if (snapshot === historySnapshot)
        return;
    historySnapshot = snapshot;
    $('downloadJobs').textContent = '';
    if (!jobs.length)
        $('downloadJobs').append(element('p', 'No downloads queued.', 'muted'));
    jobs.forEach(job => {
        const status = job.status.charAt(0).toUpperCase() + job.status.slice(1);
        const row = element('article', '', 'downloadJob'),
        header = element('div', '', 'downloadJobHeader');
        header.append(element('h3', job.name));
        if (['completed', 'failed', 'cancelled'].includes(job.status)) {
            const clear = element('button', '×', 'historyClear');
            clear.type = 'button';
            clear.setAttribute('aria-label', 'Clear ' + job.name + ' from history');
            clear.title = 'Clear history item';
            clear.onclick = () => task(clear, async() => {
                await api('downloads/clear', {
                    method: 'POST',
                    body: JSON.stringify({
                        id: job.id
                    })
                });
                await refreshDownloads();
            });
            header.append(clear);
        }
        row.append(header, element('p', status + (job.received || job.total ? ' ' + (job.status === 'downloading' && job.total ? sizeLabel(job.received) + ' / ' + sizeLabel(job.total) : sizeLabel(job.received || 0)) : '')));
        if (job.converted != null)
            row.append(element('p', 'Converted ' + sizeLabel(job.converted)));
        if (job.error)
            row.append(element('p', job.error));
        if (job.destination) {
            const path = element('p', job.destination, 'downloadPath muted');
            path.title = job.destination;
            row.append(path);
        }
        if (job.stagedPath)
            row.append(element('p', 'Verified file retained at: ' + job.stagedPath, 'muted'));
        if (['downloading', 'converting', 'inspecting'].includes(job.status)) {
            const progress = element('progress');
            setProgress(progress, job);
            row.append(progress);
        }
        if (['queued', 'downloading', 'converting', 'inspecting'].includes(job.status)) {
            const cancel = element('button', 'Cancel', 'secondary');
            cancel.type = 'button';
            cancel.onclick = () => task(cancel, async() => {
                await api('downloads/' + job.id + '/cancel', {
                    method: 'POST',
                    body: '{}'
                });
                await refreshDownloads();
            });
            row.append(cancel);
        }
        $('downloadJobs').append(row);
    });
}
function setProgress(progress, job) {
    progress.max = 1;
    progress.setAttribute('aria-label', job.name + ' · ' + job.status);
    if (job.status === 'downloading' && job.total)
        progress.value = Math.min(1, job.received / job.total);
    else
        progress.removeAttribute('value');
}
$('downloadsButton').onclick = () => task($('downloadsButton'), async() => {
    show('downloads');
    await refreshDownloads();
});
$('clearDownloads').onclick = () => task($('clearDownloads'), async() => {
    await api('downloads/clear', {
        method: 'POST',
        body: '{}'
    });
    await refreshDownloads();
});
$('downloadActivity').onclick = () => task($('downloadsButton'), async() => {
    show('downloads');
    await refreshDownloads();
});
const downloadPoll = setInterval(() => {
    if (!stopped && !document.hidden)
        refreshDownloads().catch(() => {});
}, 1000);
$('quitButton').onclick = () => task($('quitButton'), async() => {
    await api('quit', {
        method: 'POST',
        body: '{}'
    });
    stopped = true;
    clearInterval(downloadPoll);
    message();
    show('stopped', false);
    $('downloadActivity').hidden = true;
    document.querySelectorAll('header button,header a').forEach(e => {
        e.hidden = true;
    });
});
$('retryBoot').onclick = () => location.reload();
(async() => {
    try {
        await home(false);
        $('bootLoading').hidden = true;
        document.body.classList.remove('booting');
        refreshDownloads().catch(() => {});
    } catch (error) {
        $('bootLoadingText').textContent = 'Could not load PSFoil: ' + error.message;
        $('retryBoot').hidden = false;
    }
})();
