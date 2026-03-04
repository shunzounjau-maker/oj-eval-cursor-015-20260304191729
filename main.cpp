#include <bits/stdc++.h>

using namespace std;

static const size_t HEADER_SIZE = 4096;
static const size_t NODE_SIZE = 4096;
static const int MAX_KEYS = 40;
static const int32_t INVALID_NODE = -1;

struct Key {
    char index[65];  // null-terminated string, up to 64 bytes
    int32_t value;
};

static int key_compare(const Key &a, const Key &b) {
    int cmp = std::strncmp(a.index, b.index, 65);
    if (cmp < 0) return -1;
    if (cmp > 0) return 1;
    if (a.value < b.value) return -1;
    if (a.value > b.value) return 1;
    return 0;
}

static Key make_key(const std::string &idx, int32_t value) {
    Key k;
    std::memset(&k, 0, sizeof(k));
    size_t len = std::min(idx.size(), static_cast<size_t>(64));
    if (len > 0) {
        std::memcpy(k.index, idx.data(), len);
    }
    k.index[len] = '\0';
    k.value = value;
    return k;
}

#pragma pack(push, 1)
struct Header {
    char magic[8];
    int32_t version;
    int32_t root_node;
    int32_t node_count;
    char reserved[HEADER_SIZE - 8 - 4 - 4 - 4];
};

struct Node {
    uint8_t is_leaf;     // 1 for leaf, 0 for internal
    uint8_t reserved1;
    uint16_t reserved2;
    int32_t key_count;
    int32_t next_leaf;   // for leaf: next leaf node id, for internal: unused
    Key keys[MAX_KEYS];
    int32_t children[MAX_KEYS + 1];  // for internal: child node ids, for leaf: unused
};
#pragma pack(pop)

static_assert(sizeof(Header) == HEADER_SIZE, "Header size must be 4096 bytes");
static_assert(sizeof(Node) <= NODE_SIZE, "Node size must not exceed 4096 bytes");

class BPlusTree {
public:
    explicit BPlusTree(const char *filename) {
        fp = std::fopen(filename, "r+b");
        if (!fp) {
            fp = std::fopen(filename, "w+b");
            if (!fp) {
                std::perror("fopen");
                std::exit(1);
            }
            std::memset(&header, 0, sizeof(header));
            std::memcpy(header.magic, "KVSTORE", 7);
            header.version = 1;
            header.root_node = INVALID_NODE;
            header.node_count = 0;
            write_header();
        } else {
            read_header();
            if (std::strncmp(header.magic, "KVSTORE", 7) != 0) {
                // Re-initialize if file is incompatible
                std::memset(&header, 0, sizeof(header));
                std::memcpy(header.magic, "KVSTORE", 7);
                header.version = 1;
                header.root_node = INVALID_NODE;
                header.node_count = 0;
                write_header();
            }
        }
    }

    ~BPlusTree() {
        if (fp) {
            std::fflush(fp);
            std::fclose(fp);
        }
    }

    void insert(const std::string &index, int32_t value) {
        Key key = make_key(index, value);
        if (header.root_node == INVALID_NODE) {
            int32_t root_id = allocate_node();
            Node root{};
            std::memset(&root, 0, sizeof(root));
            root.is_leaf = 1;
            root.key_count = 1;
            root.next_leaf = INVALID_NODE;
            root.keys[0] = key;
            write_node(root_id, root);
            header.root_node = root_id;
            write_header();
            return;
        }

        std::vector<int32_t> path;
        int32_t leaf_id = find_leaf(key, path);
        if (leaf_id == INVALID_NODE) {
            // Should not happen if root_node is valid
            return;
        }
        Node leaf = read_node(leaf_id);
        int pos = find_key_position_in_leaf(leaf, key);
        if (pos < leaf.key_count && key_compare(leaf.keys[pos], key) == 0) {
            // Duplicate, ignore
            return;
        }
        // Insert into leaf
        for (int i = leaf.key_count; i > pos; --i) {
            leaf.keys[i] = leaf.keys[i - 1];
        }
        leaf.keys[pos] = key;
        leaf.key_count++;

        if (leaf.key_count <= MAX_KEYS) {
            write_node(leaf_id, leaf);
            return;
        }

        // Split leaf
        Node new_leaf{};
        std::memset(&new_leaf, 0, sizeof(new_leaf));
        new_leaf.is_leaf = 1;
        int split = leaf.key_count / 2;
        int right_count = leaf.key_count - split;
        for (int i = 0; i < right_count; ++i) {
            new_leaf.keys[i] = leaf.keys[split + i];
        }
        new_leaf.key_count = right_count;
        new_leaf.next_leaf = leaf.next_leaf;

        leaf.key_count = split;
        int32_t new_leaf_id = allocate_node();
        leaf.next_leaf = new_leaf_id;

        write_node(leaf_id, leaf);
        write_node(new_leaf_id, new_leaf);

        Key promote_key = new_leaf.keys[0];
        insert_into_parent(path, leaf_id, promote_key, new_leaf_id);
    }

    void erase(const std::string &index, int32_t value) {
        if (header.root_node == INVALID_NODE) return;
        Key key = make_key(index, value);
        std::vector<int32_t> path;
        int32_t leaf_id = find_leaf(key, path);
        if (leaf_id == INVALID_NODE) return;
        Node leaf = read_node(leaf_id);
        int pos = find_key_position_in_leaf(leaf, key);
        if (pos >= leaf.key_count || key_compare(leaf.keys[pos], key) != 0) {
            return;  // Not found
        }
        for (int i = pos + 1; i < leaf.key_count; ++i) {
            leaf.keys[i - 1] = leaf.keys[i];
        }
        leaf.key_count--;
        write_node(leaf_id, leaf);
        // No rebalancing for simplicity; tree remains valid for search
    }

    void find_all(const std::string &index, std::vector<int32_t> &out_values) {
        out_values.clear();
        if (header.root_node == INVALID_NODE) return;
        Key start_key = make_key(index, std::numeric_limits<int32_t>::min());
        std::vector<int32_t> path;
        int32_t leaf_id = find_leaf(start_key, path);
        if (leaf_id == INVALID_NODE) return;

        Node node = read_node(leaf_id);
        int pos = find_key_position_in_leaf(node, start_key);

        while (true) {
            while (pos < node.key_count) {
                if (std::strncmp(node.keys[pos].index, index.c_str(), 65) != 0) {
                    return;
                }
                out_values.push_back(node.keys[pos].value);
                ++pos;
            }
            if (node.next_leaf == INVALID_NODE) break;
            leaf_id = node.next_leaf;
            node = read_node(leaf_id);
            pos = 0;
        }
    }

private:
    FILE *fp{};
    Header header{};

    void read_header() {
        std::fseek(fp, 0, SEEK_SET);
        size_t n = std::fread(&header, 1, sizeof(header), fp);
        if (n != sizeof(header)) {
            std::memset(&header, 0, sizeof(header));
            std::memcpy(header.magic, "KVSTORE", 7);
            header.version = 1;
            header.root_node = INVALID_NODE;
            header.node_count = 0;
            write_header();
        }
    }

    void write_header() {
        std::fseek(fp, 0, SEEK_SET);
        std::fwrite(&header, 1, sizeof(header), fp);
        std::fflush(fp);
    }

    long long node_offset(int32_t id) const {
        return static_cast<long long>(HEADER_SIZE) +
               static_cast<long long>(id) * static_cast<long long>(NODE_SIZE);
    }

    Node read_node(int32_t id) {
        Node node{};
        std::memset(&node, 0, sizeof(node));
        std::fseek(fp, node_offset(id), SEEK_SET);
        std::fread(&node, 1, sizeof(node), fp);
        return node;
    }

    void write_node(int32_t id, const Node &node) {
        std::fseek(fp, node_offset(id), SEEK_SET);
        std::fwrite(&node, 1, sizeof(node), fp);
        std::fflush(fp);
    }

    int32_t allocate_node() {
        int32_t id = header.node_count;
        header.node_count++;
        write_header();
        return id;
    }

    int find_child_index(const Node &node, const Key &key) const {
        int i = 0;
        while (i < node.key_count && key_compare(key, node.keys[i]) >= 0) {
            ++i;
        }
        return i;
    }

    int find_key_position_in_leaf(const Node &leaf, const Key &key) const {
        int l = 0, r = leaf.key_count;
        while (l < r) {
            int m = (l + r) / 2;
            if (key_compare(leaf.keys[m], key) < 0) {
                l = m + 1;
            } else {
                r = m;
            }
        }
        return l;
    }

    int32_t find_leaf(const Key &key, std::vector<int32_t> &internal_path) {
        int32_t node_id = header.root_node;
        if (node_id == INVALID_NODE) return INVALID_NODE;
        while (true) {
            Node node = read_node(node_id);
            if (node.is_leaf) {
                return node_id;
            }
            internal_path.push_back(node_id);
            int child_idx = find_child_index(node, key);
            if (child_idx < 0) child_idx = 0;
            if (child_idx > node.key_count) child_idx = node.key_count;
            node_id = node.children[child_idx];
        }
    }

    void insert_into_parent(std::vector<int32_t> &path,
                            int32_t left_id,
                            const Key &key,
                            int32_t right_id) {
        if (path.empty()) {
            // Create new root
            int32_t root_id = allocate_node();
            Node root{};
            std::memset(&root, 0, sizeof(root));
            root.is_leaf = 0;
            root.key_count = 1;
            root.keys[0] = key;
            root.children[0] = left_id;
            root.children[1] = right_id;
            write_node(root_id, root);
            header.root_node = root_id;
            write_header();
            return;
        }

        int32_t parent_id = path.back();
        path.pop_back();
        Node parent = read_node(parent_id);

        int pos = 0;
        while (pos <= parent.key_count && parent.children[pos] != left_id) {
            ++pos;
        }
        if (pos > parent.key_count) {
            pos = parent.key_count;
        }

        for (int i = parent.key_count; i > pos; --i) {
            parent.keys[i] = parent.keys[i - 1];
        }
        for (int i = parent.key_count + 1; i > pos + 1; --i) {
            parent.children[i] = parent.children[i - 1];
        }
        parent.keys[pos] = key;
        parent.children[pos + 1] = right_id;
        parent.key_count++;

        if (parent.key_count <= MAX_KEYS) {
            write_node(parent_id, parent);
            return;
        }

        // Split internal node
        Node new_parent{};
        std::memset(&new_parent, 0, sizeof(new_parent));
        new_parent.is_leaf = 0;

        int mid = parent.key_count / 2;
        Key promote = parent.keys[mid];
        int right_keys = parent.key_count - mid - 1;

        for (int i = 0; i < right_keys; ++i) {
            new_parent.keys[i] = parent.keys[mid + 1 + i];
        }
        for (int i = 0; i < right_keys + 1; ++i) {
            new_parent.children[i] = parent.children[mid + 1 + i];
        }
        new_parent.key_count = right_keys;
        parent.key_count = mid;

        int32_t new_parent_id = allocate_node();
        write_node(parent_id, parent);
        write_node(new_parent_id, new_parent);

        insert_into_parent(path, parent_id, promote, new_parent_id);
    }
};

int main() {
    std::ios::sync_with_stdio(false);
    std::cin.tie(nullptr);

    BPlusTree db("kvstore.dat");

    int n;
    if (!(std::cin >> n)) {
        return 0;
    }
    std::string cmd;
    for (int i = 0; i < n; ++i) {
        if (!(std::cin >> cmd)) break;
        if (cmd == "insert") {
            std::string index;
            int value;
            std::cin >> index >> value;
            db.insert(index, value);
        } else if (cmd == "delete") {
            std::string index;
            int value;
            std::cin >> index >> value;
            db.erase(index, value);
        } else if (cmd == "find") {
            std::string index;
            std::cin >> index;
            std::vector<int32_t> values;
            db.find_all(index, values);
            if (values.empty()) {
                std::cout << "null\n";
            } else {
                for (size_t j = 0; j < values.size(); ++j) {
                    if (j) std::cout << ' ';
                    std::cout << values[j];
                }
                std::cout << '\n';
            }
        }
    }

    return 0;
}

