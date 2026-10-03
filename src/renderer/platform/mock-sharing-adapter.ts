import { DEMO_CLIP_URL } from './mockPreview';
import { simulateFriend } from './mock-sharing-transfer';
import type { Friend, SharedClip, SharingApi, SharingSnapshot } from '../../shared/sharing';

/** Browser preview host for the Friends tab (`?preview`, with `&sharing=off`
 * for the first-run state and `&slow` for loaders). Deterministic, in memory,
 * and never touches the network. */
export interface MockSharingOptions {
  latencyMs?: number;
  /** Public web demo: friends' clips stream this sample instead of nothing. */
  streamUrl?: string;
  seed?: Partial<SharingSnapshot>;
}

const delay = (ms: number) => new Promise<void>((resolve) => window.setTimeout(resolve, ms));
const clone = <T>(value: T): T => JSON.parse(JSON.stringify(value)) as T;
const mockCode = 'ybndrfg8ejkmcpqxot1uwisza345h769ybndrfg8ejkmcpqxot1u';

export function unsupportedSharingSnapshot(): SharingSnapshot {
  return {
    supported: false, enabled: false, appearOffline: false, status: 'off', displayName: '',
    friends: [], inbox: [], outbox: [], downloads: []
  };
}

export function createMockSharingAdapter(options: MockSharingOptions = {}): SharingApi {
  const latency = options.latencyMs ?? 0;
  const listeners = new Set<() => void>();
  let state: SharingSnapshot = {
    ...unsupportedSharingSnapshot(),
    supported: true,
    displayName: 'You',
    ...clone(options.seed ?? {})
  };
  const changed = () => { for (const listener of [...listeners]) listener(); };
  const chosenNames = new Map<string, string>();
  const commit = async (change: (draft: SharingSnapshot) => void) => {
    await delay(latency / 3);
    change(state);
    changed();
    return clone(state);
  };
  const friend = (id: string): Friend => {
    const found = state.friends.find((candidate) => candidate.id === id);
    if (!found) throw new Error('Unknown friend.');
    return found;
  };

  return {
    getState: async () => { await delay(latency); return clone(state); },
    setEnabled: (enabled) => commit((draft) => {
      draft.enabled = enabled;
      draft.status = enabled ? 'online' : 'off';
      draft.friendCode = enabled ? mockCode : null;
      draft.inviteLink = enabled ? mockInviteLink(draft.displayName) : null;
    }),
    setAppearOffline: (appearOffline) => commit((draft) => {
      draft.appearOffline = appearOffline;
      for (const friend of draft.friends) {
        if (friend.status === 'accepted') friend.presence = appearOffline ? 'unknown' : mockPresence(friend.name);
      }
    }),
    setDisplayName: (name) => commit((draft) => { draft.displayName = name.trim() || draft.displayName; }),
    addFriend: async (code, name) => {
      if (code.trim().length < 52) throw new Error('That friend code or invite link is not valid.');
      await commit((draft) => {
        draft.friends.push({ id: code.trim(), name: name.trim() || 'Friend', status: 'outgoing', addedAtMs: Date.now(), undelivered: true, presence: 'offline', nickname: null });
      });
      return 'outgoing';
    },
    acceptInvite: () => commit((draft) => {
      const invite = draft.pendingInvite;
      if (!invite) throw new Error('That invite is no longer waiting.');
      draft.pendingInvite = null;
      draft.enabled = true;
      draft.status = 'online';
      draft.friendCode = mockCode;
      draft.inviteLink = mockInviteLink(draft.displayName);
      if (!draft.friends.some((candidate) => candidate.id === invite.code)) {
        draft.friends.push({ id: invite.code, name: invite.name || 'Friend', status: 'outgoing', addedAtMs: Date.now(), undelivered: false, presence: 'offline', nickname: null });
      }
    }),
    dismissInvite: () => commit((draft) => { draft.pendingInvite = null; }),
    // The mock keeps each friend's own name beside the nickname, like the host.
    setFriendNickname: (friendId, nickname) => commit((draft) => {
      const target = friend(friendId);
      const own = chosenNames.get(friendId) ?? target.name;
      chosenNames.set(friendId, own);
      const trimmed = nickname.trim().slice(0, 40);
      target.nickname = trimmed || null;
      target.name = trimmed || own;
      for (const clip of [...draft.inbox, ...draft.outbox]) {
        if (clip.friendId === friendId) clip.friendName = target.name;
      }
    }),
    acceptFriend: (friendId) => commit(() => {
      const accepted = friend(friendId);
      accepted.status = 'accepted';
      accepted.presence = 'online';
    }),
    removeFriend: (friendId) => commit((draft) => {
      draft.friends = draft.friends.filter((candidate) => candidate.id !== friendId);
      draft.inbox = draft.inbox.filter((clip) => clip.friendId !== friendId || clip.saved);
    }),
    shareClip: async (friendId, filePath) => {
      const title = filePath.split(/[\\/]/).pop()?.replace(/\.mp4$/i, '') ?? 'Clip';
      const shareId = Math.random().toString(16).slice(2).padEnd(32, '0').slice(0, 32);
      // Hashing and preparing a clip takes a moment on the host too.
      await delay(900);
      const snapshot = await commit((draft) => {
        draft.outbox.unshift({
          shareId, friendId, friendName: friend(friendId).name, title, size: 48_000_000, durationSeconds: 30,
          resolution: '1920x1080', gameOrApp: 'Game', createdAtMs: Date.now(), sharedAtMs: Date.now(),
          saved: false, delivered: false, audioTracks: ['System audio'], streamed: [], allAudioReady: false,
          answer: 'pending', transfer: null, availableUntilMs: null, removed: false
        });
      });
      simulateFriend(() => state, shareId, changed);
      return { shareId, snapshot };
    },
    answerSharedClip: (shareId, accept) => commit((draft) => {
      const clip = draft.inbox.find((candidate) => candidate.shareId === shareId && candidate.answer === 'pending');
      if (!clip) throw new Error('That clip is no longer waiting for an answer.');
      if (accept) {
        clip.answer = 'accepted';
        clip.availableUntilMs = Date.now() + 15 * 60_000;
      }
      else draft.inbox = draft.inbox.filter((candidate) => candidate !== clip);
    }),
    revokeShare: (shareId) => commit((draft) => { draft.outbox = draft.outbox.filter((clip) => clip.shareId !== shareId); }),
    dismissSharedClip: (shareId) => commit((draft) => { draft.inbox = draft.inbox.filter((clip) => clip.shareId !== shareId); }),
    cancelDownload: (shareId) => commit((draft) => { draft.downloads = draft.downloads.filter((row) => row.shareId !== shareId); }),
    saveSharedClip: async (shareId) => {
      const clip = state.inbox.find((candidate) => candidate.shareId === shareId);
      if (!clip) throw new Error('This shared clip is no longer available.');
      const snapshot = await commit((draft) => {
        draft.downloads = draft.downloads.filter((row) => row.shareId !== shareId);
        draft.downloads.push({ shareId, receivedBytes: 0, totalBytes: clip.size, phase: 'running', bytesPerSecond: 0, relayed: false, reconnecting: false });
      });
      let received = 0;
      const timer = window.setInterval(() => {
        const row = state.downloads.find((candidate) => candidate.shareId === shareId);
        if (!row) return window.clearInterval(timer);
        received = Math.min(clip.size, received + clip.size / 12);
        row.receivedBytes = received;
        row.bytesPerSecond = clip.size / 12 / 0.25;
        if (received >= clip.size) {
          row.phase = 'done';
          clip.saved = true;
          window.clearInterval(timer);
        }
        changed();
      }, 250);
      return snapshot;
    },
    // The browser preview has no media host; the player shows its poster.
    // Like the host's runway, the streamed range grows in order from 0.
    streamUrl: async (shareId) => {
      await delay(latency);
      const timer = window.setInterval(() => {
        const clip = state.inbox.find((candidate) => candidate.shareId === shareId);
        if (!clip) return window.clearInterval(timer);
        const end = Math.min(clip.size, (clip.streamed[0]?.[1] ?? 0) + clip.size / 20);
        clip.streamed = [[0, end]];
        if (end >= clip.size) {
          clip.allAudioReady = true;
          window.clearInterval(timer);
        }
        changed();
      }, 250);
      // The preview has no audio mixer; the sample's own audio plays.
      return { video: options.streamUrl ?? '', audio: '' };
    },
    onChanged: (callback) => {
      listeners.add(callback);
      let disposed = false;
      return () => {
        if (disposed) return;
        disposed = true;
        listeners.delete(callback);
      };
    }
  };
}

const mockInviteLink = (name: string) => 'https://clipture.app/invite/#c=' + mockCode + '&n=' + encodeURIComponent(name);

const mockPresence = (name: string): Friend['presence'] => (['Maya', 'Jo', 'Kai'].includes(name) ? 'online' : 'offline');

export function mockSharingSeed(search: string): MockSharingOptions | undefined {
  const params = new URLSearchParams(search);
  if (!params.has('preview')) return undefined;
  const latencyMs = params.has('slow') ? 15000 : params.has('demo') ? 250 : 450;
  const streamUrl = params.has('demo') ? DEMO_CLIP_URL : undefined;
  const invite = params.has('invite')
    ? { code: 'w3fq8ybz1rxk5cmh7jdn4tgoe96suapiw3fq8ybz1rxk5cmh7jdn', name: 'Dana', alreadyFriends: false }
    : null;
  if (params.get('sharing') === 'off') return { latencyMs, streamUrl, seed: { pendingInvite: invite } };
  const now = Date.now();
  const hour = 3_600_000;
  const friends: Friend[] = [
    { id: 'k7qh3nfw1ecxmg8dyb5o6tzr9uaspi4jk7qh3nfw1ecxmg8dyb5o', name: 'Maya', status: 'accepted', addedAtMs: now - 90 * hour, undelivered: false, presence: 'online', nickname: null },
    { id: 'x1pqm8bwz3hfk6yrcg9eondt5sau47jix1pqm8bwz3hfk6yrcg9e', name: 'Theo', status: 'accepted', addedAtMs: now - 40 * hour, undelivered: false, presence: 'offline', nickname: null },
    { id: 'm2zkd9rqw4xbn7ycht1eog5pusa36jfim2zkd9rqw4xbn7ycht1e', name: 'Jo', status: 'accepted', addedAtMs: now - 70 * hour, undelivered: false, presence: 'online', nickname: null },
    { id: 'p8wte3jnk6rxq1bmz9yudh4gosa57cfip8wte3jnk6rxq1bmz9yu', name: 'Kai', status: 'accepted', addedAtMs: now - 12 * hour, undelivered: false, presence: 'online', nickname: null },
    { id: 'e5rbn2xhz8kqw4tmj7yco1dgusa96pfie5rbn2xhz8kqw4tmj7yc', name: 'Priya', status: 'accepted', addedAtMs: now - 200 * hour, undelivered: false, presence: 'offline', nickname: null },
    { id: 'u9djr3kx5ybe1qmcgn8zhtwo6spa74fiu9djr3kx5ybe1qmcgn8z', name: 'Rin', status: 'incoming', addedAtMs: now - hour / 3, undelivered: false, presence: 'offline', nickname: null },
    { id: 'c4hn8oyqg1tkx5bm3ejw9rzdup6as7fic4hn8oyqg1tkx5bm3ejw', name: 'Sam', status: 'outgoing', addedAtMs: now - 2 * hour, undelivered: true, presence: 'offline', nickname: null }
  ];
  const shared = (index: number, friendIndex: number, title: string, game: string, durationSeconds: number, hoursAgo: number, saved = false): SharedClip => ({
    shareId: String(index).repeat(32).slice(0, 32),
    friendId: friends[friendIndex]!.id,
    friendName: friends[friendIndex]!.name,
    title, size: durationSeconds * 1_450_000, durationSeconds, resolution: '1920x1080', gameOrApp: game,
    createdAtMs: now - hoursAgo * hour, sharedAtMs: now - hoursAgo * hour, saved, delivered: true,
    audioTracks: ['System audio', 'Microphone'], streamed: [] as [number, number][], allAudioReady: false,
    answer: 'accepted', transfer: null, availableUntilMs: now - hoursAgo * hour + 15 * 60_000, removed: false
  });
  const asking = { ...shared(5, 3, 'Ult into triple, Haven A', 'VALORANT', 38, 0.05), answer: 'pending' as const, availableUntilMs: null };
  const fresh = shared(1, 0, 'Operator flick through smoke', 'VALORANT', 45, 0.4);
  fresh.availableUntilMs = now + 11 * 60_000;
  const sent = shared(4, 1, 'Deagle ace, Mirage B', 'Counter-Strike 2', 30, 3);
  sent.transfer = { sentBytes: sent.size, totalBytes: sent.size, purpose: 'keep', state: 'complete', bytesPerSecond: 0 };
  sent.saved = true;
  return {
    latencyMs,
    streamUrl,
    seed: {
      enabled: true,
      appearOffline: false,
      status: 'online',
      friendCode: mockCode,
      inviteLink: mockInviteLink('Alex'),
      pendingInvite: invite,
      displayName: 'Alex',
      friends,
      inbox: [
        asking,
        fresh,
        shared(2, 1, 'Last circle no-scope', 'Apex Legends', 30, 5),
        shared(3, 0, 'Pentakill in the jungle', 'League of Legends', 60, 26, true)
      ],
      outbox: [sent],
      downloads: []
    }
  };
}
