// The tables of the library, as the table picker of the application shows them: favorites, display names and deletion

const TABS = [
  { id: 'all', label: 'All', empty: 'No table yet' }, // Not shown: an empty library has its own message, see render
  { id: 'recent', label: 'Recent', empty: 'No table was played yet' },
  { id: 'new', label: 'Newly added', empty: 'No table yet' },
  { id: 'played', label: 'Most played', empty: 'No table was played yet' },
  { id: 'favorites', label: 'Favorites', empty: 'No favorite yet: use the star of a table' }
];

const POLL_INTERVAL_MS = 3000; // Changes made in the application (favorites, new tables, played table) show up without reloading

const ICONS = {
  star: '<svg viewBox="0 0 24 24" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round"><polygon points="12 2 15.09 8.26 22 9.27 17 14.14 18.18 21.02 12 17.77 5.82 21.02 7 14.14 2 9.27 8.91 8.26 12 2"></polygon></svg>',
  threeDots: '<svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round"><circle cx="12" cy="12" r="1"></circle><circle cx="5" cy="12" r="1"></circle><circle cx="19" cy="12" r="1"></circle></svg>',
  rename: '<svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round"><path d="M11 4H4a2 2 0 0 0-2 2v14a2 2 0 0 0 2 2h14a2 2 0 0 0 2-2v-7"></path><path d="M18.5 2.5a2.121 2.121 0 0 1 3 3L12 15l-4 1 1-4 9.5-9.5z"></path></svg>',
  delete: '<svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round"><polyline points="3 6 5 6 21 6"></polyline><path d="M19 6v14a2 2 0 0 1-2 2H7a2 2 0 0 1-2-2V6m3 0V4a2 2 0 0 1 2-2h4a2 2 0 0 1 2 2v2"></path><line x1="10" y1="11" x2="10" y2="17"></line><line x1="14" y1="11" x2="14" y2="17"></line></svg>'
};

const State = {
  tables: [],
  scanning: false,
  lastResponse: null, // To render again only when something changed
  tab: 'all',
  search: '',
  tiles: new Map(), // uuid -> tile elements, kept between renders so that images are not loaded again
  menu: null, // Open '...' menu: { element, uuid }
  editedUuid: null // Table of the open dialog
};

const $ = (id) => document.getElementById(id);

///////////////////////////////////////////////////////////////////////////////
// Server

// Servers that require pairing answer 401 until the code displayed by the application has been entered
async function ensurePaired() {
  for (;;) {
    const response = await fetch('/info');
    if (response.status !== 401)
      return response.ok ? response.json() : {};
    const code = window.prompt('Enter the pairing code displayed by Visual Pinball');
    if (code === null) {
      document.body.textContent = 'Pairing is required to manage this device. Reload the page to try again.';
      return null;
    }
    await fetch(`/pair?code=${encodeURIComponent(code.trim())}`, { method: 'POST' });
  }
}

async function fetchTables() {
  try {
    const response = await fetch('/tables', { cache: 'no-store' });
    if (!response.ok)
      return;
    const text = await response.text();
    if (text === State.lastResponse)
      return;
    State.lastResponse = text;
    const data = JSON.parse(text);
    State.tables = data.tables || [];
    State.scanning = !!data.scanning;
    render();
  } catch (error) {
    console.error('Error fetching tables:', error);
  }
}

async function post(url) {
  const response = await fetch(url, { method: 'POST' });
  if (!response.ok)
    throw response;
  State.lastResponse = null; // Render the outcome even if it matches an older list
  await fetchTables();
}

function showStatus(message, type) {
  const status = $('tables-status');
  status.textContent = message;
  status.style.color = type === 'error' ? 'var(--danger-color)' : type === 'success' ? 'var(--success-color)' : 'var(--text-secondary)';
  clearTimeout(status.clearTimeout);
  status.clearTimeout = setTimeout(() => { status.textContent = ''; }, 4000);
}

///////////////////////////////////////////////////////////////////////////////
// List

// Same as the table picker: the characters of each word must be found in the name, in the same order, ignoring case
function matchesSearch(name, search) {
  const text = name.toLowerCase();
  return search.toLowerCase().split(' ').filter(word => word).every(word => {
    let pos = 0;
    for (const c of word) {
      pos = text.indexOf(c, pos);
      if (pos < 0)
        return false;
      pos++;
    }
    return true;
  });
}

function getVisibleTables() {
  let tables = State.tables.slice(); // Sorted by name by the server
  switch (State.tab) {
  case 'recent':
    tables = tables.filter(table => table.lastPlayedAt > 0).sort((a, b) => b.lastPlayedAt - a.lastPlayedAt);
    break;
  case 'new':
    tables.sort((a, b) => b.createdAt - a.createdAt);
    break;
  case 'played':
    tables = tables.filter(table => table.playCount > 0).sort((a, b) => b.playCount - a.playCount || b.lastPlayedAt - a.lastPlayedAt);
    break;
  case 'favorites':
    tables = tables.filter(table => table.favorite);
    break;
  }
  if (State.search)
    tables = tables.filter(table => matchesSearch(table.name, State.search));
  return tables;
}

function formatDate(secondsSinceEpoch) {
  return new Date(secondsSinceEpoch * 1000).toLocaleDateString(undefined, { day: 'numeric', month: 'short', year: 'numeric' });
}

function getStatsText(table) {
  const added = `Added on ${formatDate(table.createdAt)}`;
  if (!table.playCount || !table.lastPlayedAt)
    return `${added}\nNever played`;
  return `${added}\nLast played on ${formatDate(table.lastPlayedAt)}\nPlayed ${table.playCount === 1 ? 'once' : `${table.playCount} times`}`;
}

function getFileName(path) {
  return path.split('/').pop();
}

function renderTabs() {
  const container = $('tables-tabs');
  container.replaceChildren(...TABS.map(tab => {
    const button = document.createElement('button');
    button.type = 'button';
    button.className = 'tables-tab' + (tab.id === State.tab ? ' current' : '');
    button.textContent = tab.label;
    button.setAttribute('role', 'tab');
    button.setAttribute('aria-selected', tab.id === State.tab);
    button.onclick = () => setTab(tab.id);
    return button;
  }));
}

function setTab(tab) {
  State.tab = tab;
  history.replaceState(null, '', tab === 'all' ? location.pathname : `#${tab}`);
  renderTabs();
  render();
}

function createTile(uuid) {
  const tile = document.createElement('li');
  tile.className = 'table-tile';
  tile.dataset.uuid = uuid;
  tile.innerHTML = `
    <div class="table-tile-image">
      <span class="table-tile-placeholder"></span>
      <img alt="" loading="lazy" decoding="async">
      <span class="table-tile-badge" hidden>Playing</span>
      <button type="button" class="table-tile-star">${ICONS.star}</button>
    </div>
    <div class="table-tile-footer">
      <div class="table-tile-text">
        <div class="table-tile-name"></div>
        <div class="table-tile-file"></div>
      </div>
      <button type="button" class="table-tile-menu" title="More actions" aria-label="More actions">${ICONS.threeDots}</button>
    </div>`;
  const refs = {
    tile,
    image: tile.querySelector('img'),
    placeholder: tile.querySelector('.table-tile-placeholder'),
    badge: tile.querySelector('.table-tile-badge'),
    star: tile.querySelector('.table-tile-star'),
    name: tile.querySelector('.table-tile-name'),
    file: tile.querySelector('.table-tile-file'),
    menu: tile.querySelector('.table-tile-menu'),
    imageUrl: null
  };
  // Not hidden with display:none while loading, as browsers do not load lazy images which are not displayed
  refs.image.onload = () => { refs.image.classList.add('loaded'); refs.placeholder.hidden = true; };
  refs.image.onerror = () => { refs.image.classList.remove('loaded'); refs.placeholder.hidden = false; };
  refs.star.onclick = (e) => { e.stopPropagation(); toggleFavorite(uuid); };
  refs.menu.onclick = (e) => { e.stopPropagation(); openMenu(uuid, refs.menu); };
  return refs;
}

function updateTile(refs, table) {
  refs.name.textContent = table.name;
  refs.name.title = table.name;
  refs.file.textContent = getFileName(table.path);
  refs.file.title = table.path;
  refs.tile.title = getStatsText(table);
  refs.placeholder.textContent = (table.name.trim()[0] || '#').toUpperCase();
  refs.badge.hidden = !table.playing;
  refs.tile.classList.toggle('playing', table.playing);

  refs.star.classList.toggle('on', table.favorite);
  refs.star.setAttribute('aria-pressed', table.favorite);
  refs.star.title = table.favorite ? 'Remove from favorites' : 'Add to favorites';
  refs.star.setAttribute('aria-label', refs.star.title);

  // The modification date changes with the image, which gives a new URL: browsers may keep the images in cache
  const imageUrl = table.hasImage ? `table-image?uuid=${encodeURIComponent(table.uuid)}&v=${table.modifiedAt}` : null;
  if (imageUrl !== refs.imageUrl) {
    refs.imageUrl = imageUrl;
    if (imageUrl)
      refs.image.src = imageUrl;
    else {
      refs.image.removeAttribute('src');
      refs.image.classList.remove('loaded');
      refs.placeholder.hidden = false;
    }
  }
}

function render() {
  const tables = getVisibleTables();
  const total = State.tables.length;

  $('tables-count').textContent = State.scanning ? 'Scanning tables folder...'
    : tables.length !== total ? `${tables.length} of ${total} tables`
    : `${total} table${total === 1 ? '' : 's'}`;

  const empty = $('tables-empty');
  empty.hidden = tables.length > 0 || State.scanning;
  if (total === 0) {
    // Whatever the tab or the search, the way to add tables: the file manager
    const link = document.createElement('a');
    link.href = 'vpx.html';
    link.textContent = 'here';
    empty.replaceChildren('Upload tables and ROMs ', link);
  }
  else
    empty.textContent = State.search ? 'No table matches the search' : TABS.find(tab => tab.id === State.tab).empty;

  // Tiles are reused, and moved rather than created again, so that their images are not reloaded
  const known = new Set(State.tables.map(table => table.uuid));
  for (const uuid of [...State.tiles.keys()])
    if (!known.has(uuid))
      State.tiles.delete(uuid);
  const tiles = tables.map(table => {
    let refs = State.tiles.get(table.uuid);
    if (!refs) {
      refs = createTile(table.uuid);
      State.tiles.set(table.uuid, refs);
    }
    updateTile(refs, table);
    return refs.tile;
  });
  $('tables-grid').replaceChildren(...tiles);

  if (State.menu && !tables.some(table => table.uuid === State.menu.uuid))
    closeMenu();
}

function findTable(uuid) {
  return State.tables.find(table => table.uuid === uuid);
}

///////////////////////////////////////////////////////////////////////////////
// Actions

async function toggleFavorite(uuid) {
  const table = findTable(uuid);
  if (!table)
    return;
  // Shown at once, the list from the server confirms it
  table.favorite = !table.favorite;
  render();
  try {
    await post(`table-favorite?uuid=${encodeURIComponent(uuid)}&favorite=${table.favorite ? 1 : 0}`);
  } catch (error) {
    table.favorite = !table.favorite;
    render();
    showStatus('Failed to change the favorites', 'error');
  }
}

function openMenu(uuid, anchor) {
  const wasOpen = State.menu && State.menu.uuid === uuid;
  closeMenu();
  if (wasOpen)
    return;
  const table = findTable(uuid);
  if (!table)
    return;

  const menu = document.createElement('div');
  menu.className = 'context-menu table-menu';
  const items = [
    { icon: ICONS.rename, text: 'Display name', action: () => openNameDialog(uuid) },
    { icon: ICONS.delete, text: 'Delete', danger: true, disabled: table.playing, hint: 'The table is being played', action: () => openDeleteDialog(uuid) }
  ];
  for (const item of items) {
    const element = document.createElement('button');
    element.type = 'button';
    element.className = 'context-menu-item' + (item.danger ? ' danger' : '');
    element.disabled = !!item.disabled;
    if (item.disabled)
      element.title = item.hint;
    element.innerHTML = `<span class="context-menu-icon">${item.icon}</span><span class="context-menu-text"></span>`;
    element.querySelector('.context-menu-text').textContent = item.text;
    element.onclick = (e) => {
      e.stopPropagation();
      closeMenu();
      item.action();
    };
    menu.appendChild(element);
  }
  document.body.appendChild(menu);
  State.menu = { element: menu, uuid };
  anchor.classList.add('open');

  // Under the button, right aligned, kept in the window
  const anchorRect = anchor.getBoundingClientRect();
  const menuRect = menu.getBoundingClientRect();
  const left = Math.max(8, Math.min(anchorRect.right - menuRect.width, window.innerWidth - menuRect.width - 8));
  const top = anchorRect.bottom + 4 + menuRect.height > window.innerHeight ? anchorRect.top - 4 - menuRect.height : anchorRect.bottom + 4;
  menu.style.left = `${left}px`;
  menu.style.top = `${Math.max(8, top)}px`;
  menu.querySelector('button:not(:disabled)')?.focus({ preventScroll: true });
}

function closeMenu() {
  if (!State.menu)
    return;
  State.menu.element.remove();
  State.tiles.get(State.menu.uuid)?.menu.classList.remove('open');
  State.menu = null;
}

function openNameDialog(uuid) {
  const table = findTable(uuid);
  if (!table)
    return;
  State.editedUuid = uuid;
  const input = $('name-input');
  input.value = table.name;
  input.placeholder = table.defaultName;
  $('name-default').textContent = table.defaultName;
  $('name-dialog').showModal();
  input.select();
}

async function saveName(name) {
  const uuid = State.editedUuid;
  $('name-dialog').close();
  const table = findTable(uuid);
  if (!table || name === table.name)
    return;
  try {
    await post(`table-name?uuid=${encodeURIComponent(uuid)}&name=${encodeURIComponent(name)}`);
    showStatus(name ? `Display name set to “${name}”` : `Display name reset to “${table.defaultName}”`, 'success');
  } catch (error) {
    showStatus(error.status === 404 ? 'This table no longer exists' : 'Failed to change the display name', 'error');
    await fetchTables();
  }
}

function openDeleteDialog(uuid) {
  const table = findTable(uuid);
  if (!table)
    return;
  State.editedUuid = uuid;
  $('delete-title').textContent = `Delete “${table.name}”?`;
  $('delete-path').textContent = table.path;
  $('delete-dialog').showModal();
}

async function deleteTable() {
  const uuid = State.editedUuid;
  $('delete-dialog').close();
  const table = findTable(uuid);
  if (!table)
    return;
  showStatus(`Deleting ${table.name}...`, 'info');
  try {
    await post(`table-delete?uuid=${encodeURIComponent(uuid)}`);
    showStatus(`${table.name} was deleted`, 'success');
  } catch (error) {
    const message = error.status === 409 ? 'is being played, exit it first' : error.status === 404 ? 'no longer exists' : 'could not be deleted';
    showStatus(`${table.name} ${message}`, 'error');
    await fetchTables();
  }
}

///////////////////////////////////////////////////////////////////////////////

function setupDialogs() {
  $('name-form').onsubmit = (e) => {
    e.preventDefault();
    saveName($('name-input').value.trim());
  };
  $('name-reset').onclick = () => saveName('');
  $('name-cancel').onclick = () => $('name-dialog').close();

  $('delete-form').onsubmit = (e) => {
    e.preventDefault();
    deleteTable();
  };
  $('delete-cancel').onclick = () => $('delete-dialog').close();

  // Only their buttons close the dialogs: not a click outside of the fields (the padding of the dialog is the dialog itself, like
  // its backdrop), nor the Escape key
  for (const dialog of document.querySelectorAll('.tables-dialog'))
    dialog.addEventListener('cancel', (e) => e.preventDefault());
}

async function startup() {
  const info = await ensurePaired();
  if (info === null)
    return;
  if (info.version)
    $('version-info').textContent = info.version;
  if (info.tableLibrary !== true) {
    // The mobile launchers manage their table list themselves, and applications built before this page do not report it
    $('tables-empty').hidden = false;
    $('tables-empty').textContent = info.tableLibrary === false ? 'Tables are managed by the Visual Pinball app on this device.'
      : 'This version of Visual Pinball does not support the tables page: update it, or manage the tables from the Files page.';
    return;
  }

  const tab = location.hash.substring(1);
  State.tab = TABS.some(t => t.id === tab) ? tab : 'all';
  renderTabs();
  setupDialogs();

  $('tables-search').addEventListener('input', (e) => {
    State.search = e.target.value.trim();
    render();
  });
  document.addEventListener('click', (e) => {
    if (State.menu && !State.menu.element.contains(e.target))
      closeMenu();
  });
  document.addEventListener('keydown', (e) => {
    if (e.key === 'Escape')
      closeMenu();
  });
  window.addEventListener('resize', closeMenu);
  $('tables-grid').parentElement.addEventListener('scroll', closeMenu, { passive: true });

  await fetchTables();
  setInterval(() => {
    if (document.visibilityState === 'visible')
      fetchTables();
  }, POLL_INTERVAL_MS);
  document.addEventListener('visibilitychange', () => {
    if (document.visibilityState === 'visible')
      fetchTables();
  });
}

document.addEventListener('DOMContentLoaded', startup);
