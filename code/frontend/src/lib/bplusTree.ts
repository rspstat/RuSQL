// bplusTree.ts
// A small teaching B+Tree for the B+Tree page: every operation records its steps as frames (the whole tree as it stands after the step, the nodes the step is
// about and a short message), and the page replays the frames. It is a model of the idea (all keys live in the linked leaves, a node that holds more than
// `maxKeys` keys splits, a node that holds fewer than its minimum borrows from a sibling or merges with it), not the engine's own index.

export type Msg = { en: string; ko: string };
export type FrameKind = "ready" | "visit" | "insert" | "overflow" | "split" | "delete" | "underflow" | "borrow" | "merge" | "root" | "found" | "miss";

export interface NodeSnap { id: number; leaf: boolean; keys: number[]; children: number[]; next: number | null }
export interface Snap { root: number; nodes: Record<number, NodeSnap> }
export interface Frame { snap: Snap; kind: FrameKind; hi: number[]; msg: Msg }

interface N { id: number; leaf: boolean; keys: number[]; children: number[]; next: number | null }

export class BPlusTree {
  readonly maxKeys: number;
  private nodes = new Map<number, N>();
  private root: number;
  private nextId = 1;

  constructor(maxKeys: number) {
    this.maxKeys = maxKeys;
    this.root = this.make(true).id;
  }

  private make(leaf: boolean): N {
    const n: N = { id: this.nextId++, leaf, keys: [], children: [], next: null };
    this.nodes.set(n.id, n);
    return n;
  }

  private get(id: number): N { return this.nodes.get(id)!; }
  private get minLeaf() { return Math.ceil(this.maxKeys / 2); }
  private get minInternal() { return Math.ceil((this.maxKeys + 1) / 2) - 1; }

  snapshot(): Snap {
    const nodes: Record<number, NodeSnap> = {};
    for (const n of this.nodes.values()) nodes[n.id] = { id: n.id, leaf: n.leaf, keys: [...n.keys], children: [...n.children], next: n.next };
    return { root: this.root, nodes };
  }

  frame(kind: FrameKind, hi: number[], msg: Msg): Frame { return { snap: this.snapshot(), kind, hi, msg }; }

  has(key: number): boolean {
    let n = this.get(this.root);
    while (!n.leaf) n = this.get(n.children[this.childIndex(n, key)]);
    return n.keys.includes(key);
  }

  // where `key` goes in an internal node: the child whose range holds it
  private childIndex(n: N, key: number): number {
    let i = 0;
    while (i < n.keys.length && key >= n.keys[i]) i++;
    return i;
  }

  private range(n: N, i: number): Msg {
    const lo = i > 0 ? n.keys[i - 1] : null, hi = i < n.keys.length ? n.keys[i] : null;
    if (lo === null && hi !== null) return { en: `smaller than ${hi}`, ko: `${hi}보다 작음` };
    if (lo !== null && hi === null) return { en: `${lo} or more`, ko: `${lo} 이상` };
    return { en: `${lo} or more and smaller than ${hi}`, ko: `${lo} 이상 ${hi} 미만` };
  }

  // the path from the root to the leaf of `key`, with a frame for every node on the way
  private descend(key: number, frames: Frame[]): { leaf: N; path: { id: number; idx: number }[] } {
    const path: { id: number; idx: number }[] = [];
    let n = this.get(this.root);
    frames.push(this.frame("visit", [n.id], { en: `Start at the root to find the leaf of ${key}`, ko: `${key}가 들어갈 잎을 찾으러 루트에서 시작` }));
    while (!n.leaf) {
      const idx = this.childIndex(n, key);
      const r = this.range(n, idx);
      path.push({ id: n.id, idx });
      n = this.get(n.children[idx]);
      frames.push(this.frame("visit", [n.id], { en: `${key} is ${r.en}: go down`, ko: `${key}는 ${r.ko}: 아래로 내려감` }));
    }
    return { leaf: n, path };
  }

  search(key: number): Frame[] {
    const frames: Frame[] = [];
    const { leaf } = this.descend(key, frames);
    if (leaf.keys.includes(key)) frames.push(this.frame("found", [leaf.id], { en: `Found ${key} in this leaf`, ko: `이 잎에서 ${key}를 찾음` }));
    else frames.push(this.frame("miss", [leaf.id], { en: `${key} is not in the leaf that would hold it: it is not in the tree`, ko: `${key}가 있어야 할 잎에 없음: 트리에 없는 키` }));
    return frames;
  }

  insert(key: number): Frame[] {
    const frames: Frame[] = [];
    const { leaf, path } = this.descend(key, frames);
    if (leaf.keys.includes(key)) {
      frames.push(this.frame("miss", [leaf.id], { en: `${key} is already in the tree (keys are unique)`, ko: `${key}는 이미 있음(키는 중복 불가)` }));
      return frames;
    }
    leaf.keys.push(key);
    leaf.keys.sort((a, b) => a - b);
    frames.push(this.frame("insert", [leaf.id], { en: `Put ${key} into the leaf, in key order`, ko: `${key}를 잎에 키 순서대로 넣음` }));
    let cur = leaf;
    while (cur.keys.length > this.maxKeys) {
      frames.push(this.frame("overflow", [cur.id], { en: `The node holds ${cur.keys.length} keys, more than ${this.maxKeys}: it must split`, ko: `노드에 키가 ${cur.keys.length}개로 ${this.maxKeys}개를 넘음: 쪼개야 함` }));
      const right = this.make(cur.leaf);
      let sep: number;
      if (cur.leaf) {
        const mid = Math.ceil(cur.keys.length / 2);
        right.keys = cur.keys.splice(mid);
        right.next = cur.next;
        cur.next = right.id;
        sep = right.keys[0]; // a leaf's separator is copied up: the key stays in the leaf
      } else {
        const mid = Math.floor(cur.keys.length / 2);
        sep = cur.keys[mid]; // an internal node's separator moves up
        right.keys = cur.keys.splice(mid + 1);
        cur.keys.pop();
        right.children = cur.children.splice(mid + 1);
      }
      const parentEntry = path.pop();
      if (!parentEntry) {
        const top = this.make(false);
        top.keys = [sep];
        top.children = [cur.id, right.id];
        this.root = top.id;
        frames.push(this.frame("split", [cur.id, right.id, top.id], { en: `Split in two; ${sep} goes up into a new root: the tree grows one level`, ko: `둘로 쪼갬. ${sep}가 새 루트로 올라감: 트리가 한 층 높아짐` }));
        break;
      }
      const parent = this.get(parentEntry.id);
      const at = parent.children.indexOf(cur.id);
      parent.keys.splice(at, 0, sep);
      parent.children.splice(at + 1, 0, right.id);
      frames.push(this.frame("split", [cur.id, right.id, parent.id], { en: `Split in two; ${sep} goes up into the parent`, ko: `둘로 쪼갬. ${sep}가 부모로 올라감` }));
      cur = parent;
    }
    return frames;
  }

  remove(key: number): Frame[] {
    const frames: Frame[] = [];
    const { leaf, path } = this.descend(key, frames);
    const at = leaf.keys.indexOf(key);
    if (at < 0) {
      frames.push(this.frame("miss", [leaf.id], { en: `${key} is not in the tree`, ko: `${key}는 트리에 없음` }));
      return frames;
    }
    leaf.keys.splice(at, 1);
    frames.push(this.frame("delete", [leaf.id], { en: `Remove ${key} from the leaf`, ko: `잎에서 ${key}를 지움` }));
    let cur = leaf;
    for (;;) {
      if (cur.id === this.root) {
        if (!cur.leaf && cur.keys.length === 0) {
          this.root = cur.children[0];
          this.nodes.delete(cur.id);
          frames.push(this.frame("root", [this.root], { en: "The root has only one child left: the tree loses a level", ko: "루트에 자식이 하나만 남음: 트리가 한 층 낮아짐" }));
        }
        break;
      }
      const min = cur.leaf ? this.minLeaf : this.minInternal;
      if (cur.keys.length >= min) break;
      frames.push(this.frame("underflow", [cur.id], { en: `The node holds ${cur.keys.length} keys, fewer than ${min}: it needs help from a sibling`, ko: `노드에 키가 ${cur.keys.length}개로 ${min}개에 못 미침: 형제의 도움이 필요` }));
      const { id: parentId, idx } = path.pop()!;
      const parent = this.get(parentId);
      const left = idx > 0 ? this.get(parent.children[idx - 1]) : null;
      const right = idx < parent.children.length - 1 ? this.get(parent.children[idx + 1]) : null;
      if (left && left.keys.length > min) {
        if (cur.leaf) {
          cur.keys.unshift(left.keys.pop()!);
          parent.keys[idx - 1] = cur.keys[0];
        } else {
          cur.keys.unshift(parent.keys[idx - 1]);
          cur.children.unshift(left.children.pop()!);
          parent.keys[idx - 1] = left.keys.pop()!;
        }
        frames.push(this.frame("borrow", [left.id, cur.id, parent.id], { en: "Borrow one key from the left sibling; the separator in the parent changes", ko: "왼쪽 형제에게서 키 하나를 빌림. 부모의 구분 키가 바뀜" }));
        break;
      }
      if (right && right.keys.length > min) {
        if (cur.leaf) {
          cur.keys.push(right.keys.shift()!);
          parent.keys[idx] = right.keys[0];
        } else {
          cur.keys.push(parent.keys[idx]);
          cur.children.push(right.children.shift()!);
          parent.keys[idx] = right.keys.shift()!;
        }
        frames.push(this.frame("borrow", [cur.id, right.id, parent.id], { en: "Borrow one key from the right sibling; the separator in the parent changes", ko: "오른쪽 형제에게서 키 하나를 빌림. 부모의 구분 키가 바뀜" }));
        break;
      }
      // neither sibling can spare a key: merge with one of them and take the separator out of the parent
      let keep: N | null = null, gone: N | null = null, sepAt = 0;
      if (left) { keep = left; gone = cur; sepAt = idx - 1; }
      else if (right) { keep = cur; gone = right; sepAt = idx; }
      if (!keep || !gone) break;
      if (keep.leaf) {
        keep.keys.push(...gone.keys);
        keep.next = gone.next;
      } else {
        keep.keys.push(parent.keys[sepAt], ...gone.keys);
        keep.children.push(...gone.children);
      }
      parent.keys.splice(sepAt, 1);
      parent.children.splice(sepAt + 1, 1);
      this.nodes.delete(gone.id);
      frames.push(this.frame("merge", [keep.id, parent.id], { en: "Neither sibling can spare a key: merge two nodes into one and drop the separator from the parent", ko: "빌려줄 형제가 없음: 두 노드를 하나로 합치고 부모에서 구분 키를 뺌" }));
      cur = parent;
    }
    return frames;
  }
}
