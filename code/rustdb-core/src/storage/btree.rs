// src/storage/btree.rs

use std::cmp::Ordering;

const ORDER: usize = 4; // 노드당 최대 키 수

#[derive(Debug, Clone)]
pub enum Node {
    Internal(InternalNode),
    Leaf(LeafNode),
}

#[derive(Debug, Clone)]
pub struct InternalNode {
    pub keys: Vec<String>,
    pub children: Vec<Box<Node>>,
}

#[derive(Debug, Clone)]
pub struct LeafNode {
    pub keys: Vec<String>,
    pub values: Vec<String>, // JSON 직렬화된 Row
    // next 포인터는 분할 시 유지되지 않으므로 제거됨.
    // 범위 스캔은 트리 순회(collect_all_kv)로 수행한다.
}

#[derive(Debug)]
pub struct BPlusTree {
    root: Option<Box<Node>>,
}

/// 수치 인식 키 비교: 두 키가 모두 숫자로 파싱되면 f64 비교, 아니면 문자열 비교
/// 예) "10" > "9"  (숫자), "abc" > "aaa" (문자열)
fn cmp_keys(a: &str, b: &str) -> Ordering {
    match (a.parse::<f64>(), b.parse::<f64>()) {
        (Ok(af), Ok(bf)) => af.partial_cmp(&bf).unwrap_or(Ordering::Equal),
        _ => a.cmp(b),
    }
}

impl BPlusTree {
    pub fn new() -> Self {
        BPlusTree { root: None }
    }

    // ── 포인트 검색 ────────────────────────────────────────────────────────
    pub fn search(&self, key: &str) -> Option<String> {
        match &self.root {
            None => None,
            Some(node) => Self::search_node(node, key),
        }
    }

    fn search_node(node: &Node, key: &str) -> Option<String> {
        match node {
            Node::Leaf(leaf) => {
                for (i, k) in leaf.keys.iter().enumerate() {
                    if cmp_keys(k, key) == Ordering::Equal {
                        return Some(leaf.values[i].clone());
                    }
                }
                None
            }
            Node::Internal(internal) => {
                let idx = internal.keys.partition_point(|k| cmp_keys(k.as_str(), key) != Ordering::Greater);
                let idx = idx.min(internal.children.len() - 1);
                Self::search_node(&internal.children[idx], key)
            }
        }
    }

    // ── 삽입 ────────────────────────────────────────────────────────────────
    pub fn insert(&mut self, key: String, value: String) {
        if self.root.is_none() {
            self.root = Some(Box::new(Node::Leaf(LeafNode {
                keys: vec![key],
                values: vec![value],
            })));
            return;
        }

        let root = self.root.take().unwrap();
        let (new_root, split) = Self::insert_node(root, key, value);

        self.root = Some(match split {
            None => new_root,
            Some((mid_key, right_node)) => {
                Box::new(Node::Internal(InternalNode {
                    keys: vec![mid_key],
                    children: vec![new_root, right_node],
                }))
            }
        });
    }

    fn insert_node(
        node: Box<Node>,
        key: String,
        value: String,
    ) -> (Box<Node>, Option<(String, Box<Node>)>) {
        match *node {
            Node::Leaf(mut leaf) => {
                let pos = leaf.keys.partition_point(|k| cmp_keys(k.as_str(), key.as_str()) == Ordering::Less);
                // 이미 존재하는 키면 값 업데이트
                if pos < leaf.keys.len() && cmp_keys(&leaf.keys[pos], &key) == Ordering::Equal {
                    leaf.values[pos] = value;
                    return (Box::new(Node::Leaf(leaf)), None);
                }
                leaf.keys.insert(pos, key);
                leaf.values.insert(pos, value);

                if leaf.keys.len() >= ORDER {
                    let mid = leaf.keys.len() / 2;
                    let right_keys = leaf.keys.split_off(mid);
                    let right_values = leaf.values.split_off(mid);
                    let mid_key = right_keys[0].clone();

                    let right = Box::new(Node::Leaf(LeafNode {
                        keys: right_keys,
                        values: right_values,
                    }));

                    (Box::new(Node::Leaf(leaf)), Some((mid_key, right)))
                } else {
                    (Box::new(Node::Leaf(leaf)), None)
                }
            }

            Node::Internal(mut internal) => {
                let idx = internal.keys.partition_point(|k| cmp_keys(k.as_str(), key.as_str()) != Ordering::Greater);
                let idx = idx.min(internal.children.len() - 1);

                let child = internal.children.remove(idx);
                let (new_child, split) = Self::insert_node(child, key, value);
                internal.children.insert(idx, new_child);

                if let Some((split_key, right_child)) = split {
                    let pos = internal.keys.partition_point(|k| cmp_keys(k.as_str(), split_key.as_str()) == Ordering::Less);
                    internal.keys.insert(pos, split_key);
                    internal.children.insert(pos + 1, right_child);

                    if internal.keys.len() >= ORDER {
                        let mid = internal.keys.len() / 2;
                        let up_key = internal.keys[mid].clone();
                        let right_keys = internal.keys.split_off(mid + 1);
                        internal.keys.pop();
                        let right_children = internal.children.split_off(mid + 1);

                        let right = Box::new(Node::Internal(InternalNode {
                            keys: right_keys,
                            children: right_children,
                        }));

                        (Box::new(Node::Internal(internal)), Some((up_key, right)))
                    } else {
                        (Box::new(Node::Internal(internal)), None)
                    }
                } else {
                    (Box::new(Node::Internal(internal)), None)
                }
            }
        }
    }

    // ── 범위 검색 [start, end] ─────────────────────────────────────────────
    pub fn range_search(&self, start: &str, end: &str) -> Vec<String> {
        let mut result = Vec::new();
        if let Some(root) = &self.root {
            Self::range_collect(root, start, end, &mut result);
        }
        result
    }

    fn range_collect(node: &Node, start: &str, end: &str, result: &mut Vec<String>) {
        match node {
            Node::Leaf(leaf) => {
                for (i, k) in leaf.keys.iter().enumerate() {
                    let vs_end = cmp_keys(k, end);
                    if vs_end == Ordering::Greater { break; }
                    if cmp_keys(k, start) != Ordering::Less {
                        result.push(leaf.values[i].clone());
                    }
                }
            }
            Node::Internal(internal) => {
                for (i, child) in internal.children.iter().enumerate() {
                    if i < internal.keys.len() && cmp_keys(&internal.keys[i], start) == Ordering::Less {
                        continue;
                    }
                    if i > 0 && cmp_keys(&internal.keys[i - 1], end) == Ordering::Greater {
                        break;
                    }
                    Self::range_collect(child, start, end, result);
                }
            }
        }
    }

    // ── 개방 범위 스캔: pk >= start ────────────────────────────────────────
    /// start 이상의 모든 (key, value) 반환. inclusive=false 이면 start 제외.
    /// 내부적으로 정렬된 트리 순회 후 필터링한다.
    pub fn scan_from(&self, start: &str, inclusive: bool) -> Vec<(String, String)> {
        self.collect_all_kv().into_iter()
            .filter(|(k, _)| {
                let ord = cmp_keys(k, start);
                if inclusive { ord != Ordering::Less } else { ord == Ordering::Greater }
            })
            .collect()
    }

    // ── 개방 범위 스캔: pk <= end ──────────────────────────────────────────
    /// end 이하의 모든 (key, value) 반환. inclusive=false 이면 end 제외.
    pub fn scan_to(&self, end: &str, inclusive: bool) -> Vec<(String, String)> {
        self.collect_all_kv().into_iter()
            .filter(|(k, _)| {
                let ord = cmp_keys(k, end);
                if inclusive { ord != Ordering::Greater } else { ord == Ordering::Less }
            })
            .collect()
    }

    // ── 전체 값 / (키, 값) ───────────────────────────────────────────────
    pub fn all_values(&self) -> Vec<String> {
        let mut result = Vec::new();
        if let Some(root) = &self.root {
            Self::collect_all(root, &mut result);
        }
        result
    }

    fn collect_all(node: &Node, result: &mut Vec<String>) {
        match node {
            Node::Leaf(leaf) => result.extend(leaf.values.clone()),
            Node::Internal(internal) => {
                for child in &internal.children {
                    Self::collect_all(child, result);
                }
            }
        }
    }

    /// 정렬된 순서로 모든 (key, value) 반환
    pub fn collect_all_kv(&self) -> Vec<(String, String)> {
        let mut result = Vec::new();
        if let Some(root) = &self.root {
            Self::collect_kv_node(root, &mut result);
        }
        result
    }

    fn collect_kv_node(node: &Node, result: &mut Vec<(String, String)>) {
        match node {
            Node::Leaf(leaf) => {
                for (i, k) in leaf.keys.iter().enumerate() {
                    result.push((k.clone(), leaf.values[i].clone()));
                }
            }
            Node::Internal(internal) => {
                for child in &internal.children {
                    Self::collect_kv_node(child, result);
                }
            }
        }
    }

    // ── 통계 ────────────────────────────────────────────────────────────
    /// 트리에 저장된 키(행) 수
    pub fn len(&self) -> usize {
        self.all_values().len()
    }

    pub fn is_empty(&self) -> bool {
        self.root.is_none()
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn test_numeric_cmp() {
        assert_eq!(cmp_keys("10", "9"), Ordering::Greater);
        assert_eq!(cmp_keys("2", "10"), Ordering::Less);
        assert_eq!(cmp_keys("5", "5"), Ordering::Equal);
        assert_eq!(cmp_keys("abc", "abd"), Ordering::Less);
    }

    #[test]
    fn test_insert_search() {
        let mut tree = BPlusTree::new();
        for i in [1, 10, 5, 3, 7, 2, 8, 4, 6, 9] {
            tree.insert(i.to_string(), format!("v{}", i));
        }
        assert_eq!(tree.search("10"), Some("v10".to_string()));
        assert_eq!(tree.search("1"), Some("v1".to_string()));
        assert_eq!(tree.search("11"), None);
    }

    #[test]
    fn test_range_search() {
        let mut tree = BPlusTree::new();
        for i in 1..=10 {
            tree.insert(i.to_string(), format!("v{}", i));
        }
        let r = tree.range_search("3", "7");
        assert_eq!(r.len(), 5); // 3,4,5,6,7
    }

    #[test]
    fn test_scan_from() {
        let mut tree = BPlusTree::new();
        for i in 1..=10 {
            tree.insert(i.to_string(), format!("v{}", i));
        }
        let r = tree.scan_from("8", true);
        let keys: Vec<&str> = r.iter().map(|(k, _)| k.as_str()).collect();
        assert_eq!(keys, vec!["8", "9", "10"]);

        let r2 = tree.scan_from("8", false);
        let keys2: Vec<&str> = r2.iter().map(|(k, _)| k.as_str()).collect();
        assert_eq!(keys2, vec!["9", "10"]);
    }

    #[test]
    fn test_scan_to() {
        let mut tree = BPlusTree::new();
        for i in 1..=10 {
            tree.insert(i.to_string(), format!("v{}", i));
        }
        let r = tree.scan_to("3", true);
        let keys: Vec<&str> = r.iter().map(|(k, _)| k.as_str()).collect();
        assert_eq!(keys, vec!["1", "2", "3"]);

        let r2 = tree.scan_to("3", false);
        let keys2: Vec<&str> = r2.iter().map(|(k, _)| k.as_str()).collect();
        assert_eq!(keys2, vec!["1", "2"]);
    }
}
