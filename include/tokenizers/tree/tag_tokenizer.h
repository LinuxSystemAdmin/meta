/**
 * @file tag_tokenizer.h
 *
 * All files in META are released under the MIT license. For more details,
 * consult the file LICENSE in the root of the project.
 */

#ifndef _META_TOKENIZERS_TAG_TOKENIZER_H_
#define _META_TOKENIZERS_TAG_TOKENIZER_H_

#include "tokenizers/tree/tree_tokenizer.h"

namespace meta {
namespace tokenizers {

/**
 * Tokenizes parse trees by looking at labels of leaf and interior nodes.
 */
class tag_tokenizer : public tree_tokenizer<tag_tokenizer> {
    public:
        using mapping_fn = std::function<term_id(const std::string &)>;

        /**
         * Counts occurrences of leaf and interior node labels.
         * @param document - the document to parse
         * @param tree - the current parse_tree in the document
         */
        void tree_tokenize(index::document & document,
                           const parse_tree & tree, 
                           mapping_fn mapping);
};

}
}

#endif