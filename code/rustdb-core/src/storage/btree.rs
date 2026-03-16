 // src/storage/btree.rs

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
    pub next: Option<Box<LeafNode>>, // 리프 연결 리스트
}

#[derive(Debug)]
pub struct BPlusTree {
    root: Option<Box<Node>>,
}

impl BPlusTree {
    pub fn new() -> Self {
        BPlusTree { root: None }
    }

    // 검색
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
                    if k == key {
                        return Some(leaf.values[i].clone());
                    }
                }
                None
            }
            Node::Internal(internal) => {
                let idx = internal.keys.partition_point(|k| k.as_str() <= key);
                let idx = idx.min(internal.children.len() - 1);
                Self::search_node(&internal.children[idx], key)
            }
        }
    }

    // 삽입
    pub fn insert(&mut self, key: String, value: String) {
        if self.root.is_none() {
            self.root = Some(Box::new(Node::Leaf(LeafNode {
                keys: vec![key],
                values: vec![value],
                next: None,
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
                let pos = leaf.keys.partition_point(|k| k.as_str() < key.as_str());
                leaf.keys.insert(pos, key);
                leaf.values.insert(pos, value);

                if leaf.keys.len() >= ORDER {
                    // 분할
                    let mid = leaf.keys.len() / 2;
                    let right_keys = leaf.keys.split_off(mid);
                    let right_values = leaf.values.split_off(mid);
                    let mid_key = right_keys[0].clone();

                    let right = Box::new(Node::Leaf(LeafNode {
                        keys: right_keys,
                        values: right_values,
                        next: leaf.next.take(),
                    }));

                    (Box::new(Node::Leaf(leaf)), Some((mid_key, right)))
                } else {
                    (Box::new(Node::Leaf(leaf)), None)
                }
            }

            Node::Internal(mut internal) => {
                let idx = internal.keys.partition_point(|k| k.as_str() <= key.as_str());
                let idx = idx.min(internal.children.len() - 1);

                let child = internal.children.remove(idx);
                let (new_child, split) = Self::insert_node(child, key, value);
                internal.children.insert(idx, new_child);

                if let Some((split_key, right_child)) = split {
                    let pos = internal.keys.partition_point(|k| k.as_str() < split_key.as_str());
                    internal.keys.insert(pos, split_key);
                    internal.children.insert(pos + 1, right_child);

                    if internal.keys.len() >= ORDER {
                        // 내부 노드 분할
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

    // 범위 검색 (리프 연결 리스트 활용)
    pub fn range_search(&self, start: &str, end: &str) -> Vec<String> {
        let mut result = Vec::new();
        if let Some(root) = &self.root {
            let mut node = root.as_ref();
            // 시작 리프 노드 찾기
            loop {
                match node {
                    Node::Internal(internal) => {
                        let idx = internal.keys.partition_point(|k| k.as_str() <= start);
                        let idx = idx.min(internal.children.len() - 1);
                        node = &internal.children[idx];
                    }
                    Node::Leaf(leaf) => {
                        for (i, k) in leaf.keys.iter().enumerate() {
                            if k.as_str() >= start && k.as_str() <= end {
                                result.push(leaf.values[i].clone());
                            }
                        }
                        break;
                    }
                }
            }
        }
        result
    }

    // 전체 키 목록
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
}
