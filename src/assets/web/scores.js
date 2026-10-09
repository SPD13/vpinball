// The scores recorded on the device at the end of the games, by table and by player, and the players (profiles) they belong to

const VIEWS = [
  { id: 'all', label: 'All scores' },
  { id: 'best', label: 'Best per player' }
];

const POLL_INTERVAL_MS = 3000; // Games ended in the application show up without reloading

const ICONS = {
  assign: '<svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round"><path d="M20 21v-2a4 4 0 0 0-4-4H8a4 4 0 0 0-4 4v2"></path><circle cx="12" cy="7" r="4"></circle></svg>',
  rename: '<svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round"><path d="M11 4H4a2 2 0 0 0-2 2v14a2 2 0 0 0 2 2h14a2 2 0 0 0 2-2v-7"></path><path d="M18.5 2.5a2.121 2.121 0 0 1 3 3L12 15l-4 1 1-4 9.5-9.5z"></path></svg>',
  delete: '<svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round"><polyline points="3 6 5 6 21 6"></polyline><path d="M19 6v14a2 2 0 0 1-2 2H7a2 2 0 0 1-2-2V6m3 0V4a2 2 0 0 1 2-2h4a2 2 0 0 1 2 2v2"></path><line x1="10" y1="11" x2="10" y2="17"></line><line x1="14" y1="11" x2="14" y2="17"></line></svg>'
};

// How the score was read, for the tooltip of the rows (see ScoreTracker in the application)
const SOURCES = {
  pinmame: 'the memory of the machine (PinMAME)',
  b2s: 'the backglass (B2S)',
  ultradmd: 'the UltraDMD scoreboard',
  script: 'the table script',
  highscore: 'the high scores saved by the table'
};

const UNASSIGNED = '-'; // Player filter value for the scores that belong to nobody

const CLEAR_ALL_CONFIRMATION = 'DELETE'; // To type before clearing all the scores, as it can not be undone

const State = {
  profiles: [],
  tables: [],
  scores: [],
  activeProfileId: '',
  players: [], // Profile id of each player of the games (index 0 is player 1), empty if unassigned
  lastResponse: null, // To render again only when something changed
  tableUuid: '', // Empty for all tables
  profileFilter: '', // Empty for all players, UNASSIGNED, or a profile id
  view: 'all',
  editedScoreId: null, // Score of the open dialog
  clearedTableUuid: null, // Table of the open clear dialog (empty for all tables)
  editedProfileId: null // Profile of the open dialog (null when adding one)
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

async function fetchScores() {
  try {
    const response = await fetch('/scores', { cache: 'no-store' });
    if (!response.ok)
      return;
    const text = await response.text();
    if (text === State.lastResponse)
      return;
    State.lastResponse = text;
    const data = JSON.parse(text);
    State.profiles = data.profiles || [];
    State.tables = data.tables || [];
    State.scores = data.scores || [];
    State.activeProfileId = data.activeProfileId || '';
    State.players = data.players || [State.activeProfileId];
    render();
  } catch (error) {
    console.error('Error fetching scores:', error);
  }
}

async function post(url) {
  const response = await fetch(url, { method: 'POST' });
  if (!response.ok)
    throw response;
  State.lastResponse = null; // Render the outcome even if it matches an older list
  await fetchScores();
  return response;
}

function showStatus(message, type) {
  const status = $('scores-status');
  status.textContent = message;
  status.style.color = type === 'error' ? 'var(--danger-color)' : type === 'success' ? 'var(--success-color)' : 'var(--text-secondary)';
  clearTimeout(status.clearTimeout);
  status.clearTimeout = setTimeout(() => { status.textContent = ''; }, 4000);
}

///////////////////////////////////////////////////////////////////////////////
// Helpers

function findProfile(id) {
  return State.profiles.find(profile => profile.id === id);
}

function findTable(uuid) {
  return State.tables.find(table => table.uuid === uuid);
}

function findScore(id) {
  return State.scores.find(score => score.id === id);
}

function getTableName(uuid) {
  return findTable(uuid)?.name || 'Unknown table';
}

function getPlayerName(score) {
  return findProfile(score.profileId)?.name || 'Unassigned';
}

function formatScore(score) {
  return Number(score).toLocaleString();
}

function formatDate(secondsSinceEpoch) {
  return new Date(secondsSinceEpoch * 1000).toLocaleString(undefined, { day: 'numeric', month: 'short', year: 'numeric', hour: '2-digit', minute: '2-digit' });
}

function describeScore(score) {
  return `${formatScore(score.score)} on ${getTableName(score.tableUuid)}, ${formatDate(score.playedAt)} (player ${score.playerSlot}${score.playerCount > 1 ? ` of ${score.playerCount}` : ''})`;
}

///////////////////////////////////////////////////////////////////////////////
// List

// The scores to show with their rank: on their table among all scores (given by the server), or among the best score of each player
function getVisibleScores() {
  let scores = State.scores; // Best first
  if (State.tableUuid)
    scores = scores.filter(score => score.tableUuid === State.tableUuid);
  if (State.view === 'best') {
    const seen = new Set();
    const ranks = new Map();
    scores = scores.filter(score => {
      const key = `${score.tableUuid}/${score.profileId}`;
      if (!score.profileId || seen.has(key))
        return false;
      seen.add(key);
      return true;
    }).map(score => {
      const rank = (ranks.get(score.tableUuid) || 0) + 1;
      ranks.set(score.tableUuid, rank);
      return { ...score, rank };
    });
  }
  if (State.profileFilter === UNASSIGNED)
    scores = scores.filter(score => !score.profileId);
  else if (State.profileFilter)
    scores = scores.filter(score => score.profileId === State.profileFilter);
  // Ranks are per table: the leaderboards of all the tables follow each other, by table name
  if (!State.tableUuid)
    scores = scores.slice().sort((a, b) => getTableName(a.tableUuid).localeCompare(getTableName(b.tableUuid), undefined, { sensitivity: 'base' })
      || a.tableUuid.localeCompare(b.tableUuid) || a.rank - b.rank);
  return scores;
}

function setOptions(select, options, value) {
  select.replaceChildren(...options.map(option => {
    const element = document.createElement('option');
    element.value = option.value;
    element.textContent = option.label;
    return element;
  }));
  select.value = options.some(option => option.value === value) ? value : '';
}

function renderFilters() {
  // Tables with scores, and the one asked for (from the tables page) even without any
  const withScores = new Set(State.scores.map(score => score.tableUuid));
  const tables = State.tables.filter(table => withScores.has(table.uuid) || table.uuid === State.tableUuid)
    .sort((a, b) => a.name.localeCompare(b.name, undefined, { sensitivity: 'base' }));
  setOptions($('scores-table'), [{ value: '', label: 'All tables' }, ...tables.map(table => ({ value: table.uuid, label: table.inLibrary ? table.name : `${table.name} (removed)` }))], State.tableUuid);
  State.tableUuid = $('scores-table').value; // A table that no longer exists shows all of them

  const profiles = State.profiles.map(profile => ({ value: profile.id, label: profile.name }));
  if (State.view === 'all')
    profiles.push({ value: UNASSIGNED, label: 'Unassigned' });
  setOptions($('scores-profile'), [{ value: '', label: 'All players' }, ...profiles], State.profileFilter);
  State.profileFilter = $('scores-profile').value;

  $('scores-views').replaceChildren(...VIEWS.map(view => {
    const button = document.createElement('button');
    button.type = 'button';
    button.className = 'tables-tab' + (view.id === State.view ? ' current' : '');
    button.textContent = view.label;
    button.setAttribute('role', 'tab');
    button.setAttribute('aria-selected', view.id === State.view);
    button.onclick = () => {
      State.view = view.id;
      updateUrl();
      render();
    };
    return button;
  }));
}

function createIconButton(icon, title, action, danger) {
  const button = document.createElement('button');
  button.type = 'button';
  button.className = 'scores-icon-button' + (danger ? ' danger' : '');
  button.title = title;
  button.setAttribute('aria-label', title);
  button.innerHTML = icon;
  button.onclick = action;
  return button;
}

function renderRows(scores) {
  const allTables = !State.tableUuid;
  $('scores-table-list').classList.toggle('all-tables', allTables);
  $('scores-rows').replaceChildren(...scores.map(score => {
    const row = document.createElement('tr');
    row.classList.toggle('active-player', !!score.profileId && score.profileId === State.activeProfileId);
    row.classList.toggle('unassigned', !score.profileId);
    row.title = `Read from ${SOURCES[score.source] || score.source}`;

    const rank = document.createElement('td');
    rank.className = 'scores-rank';
    rank.textContent = score.rank;
    rank.classList.toggle('top', score.rank <= 3);

    const value = document.createElement('td');
    value.className = 'scores-value';
    value.textContent = formatScore(score.score);

    const player = document.createElement('td');
    const name = document.createElement('div');
    name.className = 'scores-player';
    name.textContent = getPlayerName(score);
    player.appendChild(name);
    if (score.playerCount > 1) {
      const slot = document.createElement('div');
      slot.className = 'scores-detail';
      slot.textContent = `Player ${score.playerSlot} of ${score.playerCount}`;
      player.appendChild(slot);
    }

    const table = document.createElement('td');
    table.className = 'scores-table-name';
    table.textContent = getTableName(score.tableUuid);

    const date = document.createElement('td');
    date.className = 'scores-date';
    date.textContent = formatDate(score.playedAt);

    const actions = document.createElement('td');
    actions.className = 'scores-actions';
    actions.append(
      createIconButton(ICONS.assign, 'Give to another player', () => openAssignDialog(score.id)),
      createIconButton(ICONS.delete, 'Delete', () => openScoreDeleteDialog(score.id), true));

    row.append(rank, value, player, table, date, actions);
    return row;
  }));
}

function renderProfiles() {
  const list = $('profiles-list');
  if (State.profiles.length === 0) {
    const empty = document.createElement('li');
    empty.className = 'profiles-empty';
    empty.textContent = 'No player yet: add one here, or in the lobby of the application.';
    list.replaceChildren(empty);
    return;
  }
  list.replaceChildren(...State.profiles.map(profile => {
    const item = document.createElement('li');
    item.className = 'profile-item' + (profile.id === State.activeProfileId ? ' active' : '');
    const slots = State.players.flatMap((id, index) => id === profile.id ? [index + 1] : []); // Players of the games given this profile in the lobby

    const text = document.createElement('div');
    text.className = 'profile-text';
    const name = document.createElement('div');
    name.className = 'profile-name';
    name.textContent = profile.name;
    const details = document.createElement('div');
    details.className = 'scores-detail';
    details.textContent = `${profile.scoreCount} score${profile.scoreCount === 1 ? '' : 's'}`;
    text.append(name, details);

    const state = document.createElement('div');
    state.className = 'profile-state';
    if (slots.length > 0) {
      const badge = document.createElement('span');
      badge.className = 'profile-badge';
      badge.textContent = `Player ${slots.join(', ')}`;
      state.appendChild(badge);
    }
    if (profile.id !== State.activeProfileId) {
      const activate = document.createElement('button');
      activate.type = 'button';
      activate.className = 'btn btn-secondary profile-activate';
      activate.textContent = 'Set as player 1';
      activate.onclick = () => setActiveProfile(profile.id);
      state.appendChild(activate);
    }
    state.append(
      createIconButton(ICONS.rename, 'Rename', () => openProfileNameDialog(profile.id)),
      createIconButton(ICONS.delete, 'Delete', () => openProfileDeleteDialog(profile.id), true));

    item.append(text, state);
    return item;
  }));
}

function render() {
  renderFilters();
  const scores = getVisibleScores();
  const total = State.tableUuid ? State.scores.filter(score => score.tableUuid === State.tableUuid).length : State.scores.length;
  $('scores-count').textContent = scores.length !== total ? `${scores.length} of ${total} scores` : `${total} score${total === 1 ? '' : 's'}`;

  const empty = $('scores-empty');
  empty.hidden = scores.length > 0;
  $('scores-table-list').hidden = scores.length === 0;
  if (State.scores.length === 0)
    empty.textContent = 'No score yet: scores are recorded at the end of each game played on a table of the library.';
  else if (total === 0)
    empty.textContent = 'No score yet on this table.';
  else
    empty.textContent = 'No score matches the filters.';

  $('scores-clear-table').hidden = !State.tableUuid || total === 0;
  $('scores-clear-all').hidden = State.scores.length === 0;

  renderRows(scores);
  renderProfiles();
}

function updateUrl() {
  const params = new URLSearchParams();
  if (State.tableUuid)
    params.set('uuid', State.tableUuid);
  if (State.view !== 'all')
    params.set('view', State.view);
  const query = params.toString();
  history.replaceState(null, '', query ? `?${query}` : location.pathname);
}

///////////////////////////////////////////////////////////////////////////////
// Scores

function openScoreDeleteDialog(id) {
  const score = findScore(id);
  if (!score)
    return;
  State.editedScoreId = id;
  $('score-delete-details').textContent = `${getPlayerName(score)}: ${describeScore(score)}`;
  $('score-delete-dialog').showModal();
}

async function deleteScore() {
  const score = findScore(State.editedScoreId);
  $('score-delete-dialog').close();
  if (!score)
    return;
  try {
    await post(`score-delete?id=${encodeURIComponent(score.id)}`);
    showStatus(`Score of ${formatScore(score.score)} deleted`, 'success');
  } catch (error) {
    showStatus(error.status === 404 ? 'This score no longer exists' : 'Failed to delete the score', 'error');
    await fetchScores();
  }
}

// Empty uuid: the scores of all the tables
function openClearDialog(uuid) {
  const scores = uuid ? State.scores.filter(score => score.tableUuid === uuid) : State.scores;
  if (scores.length === 0)
    return;
  State.clearedTableUuid = uuid;
  const count = `${scores.length} score${scores.length === 1 ? '' : 's'}`;
  const players = new Set(scores.map(score => score.profileId)).size;
  const tables = new Set(scores.map(score => score.tableUuid)).size;
  $('scores-clear-title').textContent = uuid ? 'Clear the scores of this table?' : 'Clear all the scores?';
  $('scores-clear-details').textContent = uuid ? `${getTableName(uuid)}: ${count}` : `${count} on ${tables} table${tables === 1 ? '' : 's'}`;
  $('scores-clear-hint').textContent = `The ${scores.length === 1 ? 'score' : `${count}`}${players > 1 ? ' of all the players' : ''} will be removed from the leaderboard${uuid ? '' : 's'}, whatever the filters. The players are kept. This can not be undone.`;
  $('scores-clear-confirm').hidden = !!uuid;
  $('scores-clear-input').value = '';
  updateClearSubmit();
  $('scores-clear-dialog').showModal();
  if (uuid)
    $('scores-clear-cancel').focus(); // Not the destructive button, so that Enter does not clear by mistake
  else
    $('scores-clear-input').focus();
}

function updateClearSubmit() {
  $('scores-clear-submit').disabled = !State.clearedTableUuid && $('scores-clear-input').value.trim().toUpperCase() !== CLEAR_ALL_CONFIRMATION;
}

async function clearScores() {
  const uuid = State.clearedTableUuid;
  if ($('scores-clear-submit').disabled)
    return;
  const tableName = getTableName(uuid); // Before the refresh, which forgets the removed tables once they have no score
  $('scores-clear-dialog').close();
  try {
    const response = await post(uuid ? `scores-clear?uuid=${encodeURIComponent(uuid)}` : 'scores-clear?all=1');
    const { deleted } = await response.json();
    showStatus(`${deleted} score${deleted === 1 ? '' : 's'} deleted${uuid ? ` from ${tableName}` : ''}`, 'success');
  } catch (error) {
    showStatus('Failed to clear the scores', 'error');
    await fetchScores();
  }
}

function openAssignDialog(id) {
  const score = findScore(id);
  if (!score)
    return;
  State.editedScoreId = id;
  $('score-assign-details').textContent = describeScore(score);
  setOptions($('score-assign-profile'), [...State.profiles.map(profile => ({ value: profile.id, label: profile.name })), { value: '', label: 'Nobody (unassigned)' }], score.profileId);
  $('score-assign-dialog').showModal();
  $('score-assign-profile').focus();
}

async function assignScore() {
  const score = findScore(State.editedScoreId);
  const profileId = $('score-assign-profile').value;
  $('score-assign-dialog').close();
  if (!score || profileId === score.profileId)
    return;
  try {
    await post(`score-assign?id=${encodeURIComponent(score.id)}&profile=${encodeURIComponent(profileId)}`);
    showStatus(profileId ? `Score given to ${findProfile(profileId)?.name}` : 'Score unassigned', 'success');
  } catch (error) {
    showStatus(error.status === 404 ? 'This score or player no longer exists' : 'Failed to change the player of the score', 'error');
    await fetchScores();
  }
}

///////////////////////////////////////////////////////////////////////////////
// Profiles

async function setActiveProfile(id) {
  try {
    await post(`profile-active?id=${encodeURIComponent(id)}`);
    showStatus(`${findProfile(id)?.name} is now player 1`, 'success');
  } catch (error) {
    showStatus('Failed to change player 1', 'error');
    await fetchScores();
  }
}

function openProfileNameDialog(id) {
  const profile = id ? findProfile(id) : null;
  State.editedProfileId = profile ? profile.id : null;
  $('profile-name-title').textContent = profile ? `Rename ${profile.name}` : 'Add player';
  $('profile-name-input').value = profile ? profile.name : '';
  $('profile-name-error').hidden = true;
  $('profile-name-dialog').showModal();
  $('profile-name-input').select();
}

async function saveProfileName() {
  const name = $('profile-name-input').value.trim();
  const error = $('profile-name-error');
  if (!name) {
    error.textContent = 'Enter a name';
    error.hidden = false;
    return;
  }
  const id = State.editedProfileId;
  const profile = id ? findProfile(id) : null;
  if (profile && profile.name === name) {
    $('profile-name-dialog').close();
    return;
  }
  try {
    if (id)
      await post(`profile-rename?id=${encodeURIComponent(id)}&name=${encodeURIComponent(name)}`);
    else
      await post(`profile-add?name=${encodeURIComponent(name)}`);
    $('profile-name-dialog').close();
    showStatus(id ? `Renamed to ${name}` : `${name} added`, 'success');
  } catch (response) {
    // The dialog stays open to fix the name
    error.textContent = response.status === 409 ? 'Another player has this name' : response.status === 404 ? 'This player no longer exists' : 'Failed to save the name';
    error.hidden = false;
  }
}

function openProfileDeleteDialog(id) {
  const profile = findProfile(id);
  if (!profile)
    return;
  State.editedProfileId = id;
  $('profile-delete-title').textContent = `Delete ${profile.name}?`;
  $('profile-delete-hint').textContent = profile.scoreCount === 0 ? 'This player has no score.'
    : `Their ${profile.scoreCount === 1 ? 'score is' : `${profile.scoreCount} scores are`} kept, unassigned: they can be given to another player.`;
  $('profile-delete-dialog').showModal();
}

async function deleteProfile() {
  const profile = findProfile(State.editedProfileId);
  $('profile-delete-dialog').close();
  if (!profile)
    return;
  try {
    await post(`profile-delete?id=${encodeURIComponent(profile.id)}`);
    showStatus(`${profile.name} deleted`, 'success');
  } catch (error) {
    showStatus(error.status === 404 ? 'This player no longer exists' : 'Failed to delete the player', 'error');
    await fetchScores();
  }
}

///////////////////////////////////////////////////////////////////////////////

function setupDialogs() {
  const bind = (name, submit) => {
    $(`${name}-form`).onsubmit = (e) => {
      e.preventDefault();
      submit();
    };
    $(`${name}-cancel`).onclick = () => $(`${name}-dialog`).close();
  };
  bind('score-delete', deleteScore);
  bind('scores-clear', clearScores);
  $('scores-clear-input').addEventListener('input', updateClearSubmit);
  bind('score-assign', assignScore);
  bind('profile-name', saveProfileName);
  bind('profile-delete', deleteProfile);

  // Only their buttons close the dialogs: not a click outside of the fields, nor the Escape key
  for (const dialog of document.querySelectorAll('.tables-dialog'))
    dialog.addEventListener('cancel', (e) => e.preventDefault());
}

async function startup() {
  const info = await ensurePaired();
  if (info === null)
    return;
  if (info.version)
    $('version-info').textContent = info.version;
  if (info.scores !== true) {
    $('scores-empty').hidden = false;
    $('scores-table-list').hidden = true;
    $('scores-empty').textContent = info.tableLibrary === false ? 'Scores are not recorded by the Visual Pinball app on this device.'
      : 'This version of Visual Pinball does not record scores: update it.';
    return;
  }

  const params = new URLSearchParams(location.search);
  State.tableUuid = params.get('uuid') || '';
  State.view = VIEWS.some(view => view.id === params.get('view')) ? params.get('view') : 'all';
  setupDialogs();

  $('scores-table').addEventListener('change', (e) => {
    State.tableUuid = e.target.value;
    updateUrl();
    render();
  });
  $('scores-profile').addEventListener('change', (e) => {
    State.profileFilter = e.target.value;
    render();
  });
  $('profile-add').onclick = () => openProfileNameDialog(null);
  $('scores-clear-table').onclick = () => openClearDialog(State.tableUuid);
  $('scores-clear-all').onclick = () => openClearDialog('');

  await fetchScores();
  setInterval(() => {
    if (document.visibilityState === 'visible')
      fetchScores();
  }, POLL_INTERVAL_MS);
  document.addEventListener('visibilitychange', () => {
    if (document.visibilityState === 'visible')
      fetchScores();
  });
}

document.addEventListener('DOMContentLoaded', startup);
