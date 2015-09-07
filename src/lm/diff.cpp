/**
 * @file diff.cpp
 * @author Sean Massung
 */

#include <algorithm>
#include <queue>
#include "lm/diff.h"
#include "porter2_stemmer.h"
#include "utf/utf.h"
#include "util/fixed_heap.h"

namespace meta
{
namespace lm
{
diff::diff(const cpptoml::table& config) : lm_{config}
{
    auto table = config.get_table("diff");
    if (!table)
        throw diff_exception{"missing [diff] table from config"};

    auto nval = table->get_as<int64_t>("n-value");
    if (!nval)
        throw diff_exception{"n-value not specified in config"};
    n_val_ = *nval;

    auto edits = table->get_as<int64_t>("max-edits");
    if (!edits)
        throw diff_exception{"max-edits not specified in config"};
    max_edits_ = *edits;

    auto b_pen = table->get_as<double>("base-penalty");
    base_penalty_ = b_pen ? *b_pen : 0.0;

    auto i_pen = table->get_as<double>("insert-penalty");
    insert_penalty_ = i_pen ? *i_pen : 0.0;

    auto s_pen = table->get_as<double>("substitute-penalty");
    substitute_penalty_ = s_pen ? *s_pen : 0.0;

    auto r_pen = table->get_as<double>("remove-penalty");
    remove_penalty_ = r_pen ? *r_pen : 0.0;

    auto max_cand = table->get_as<int64_t>("max-candidates");
    max_cand_size_ = max_cand ? *max_cand : 20;

    auto lambda = table->get_as<double>("lambda");
    lambda_ = lambda ? *lambda : 0.5;
    if (lambda_ < 0.0 || lambda_ > 1.0)
        throw diff_exception{"lambda value has to be on [0,1]"};

    auto lm_gen = table->get_as<bool>("lm-generate");
    lm_generate_ = lm_gen ? *lm_gen : false;

    set_stems(*table);
    set_function_words(*table);
}

std::vector<std::pair<sentence, double>>
    diff::candidates(const sentence& sent, bool use_lm /* = false */)
{
    use_lm_ = use_lm;
    using pair_t = std::pair<sentence, double>;
    auto comp = [](const pair_t& a, const pair_t& b)
    {
        return a.second < b.second;
    };

    util::fixed_heap<pair_t, decltype(comp)> candidates{max_cand_size_, comp};
    seen_.clear();
    add(candidates, sent);
    step(sent, candidates, 0);
    return candidates.reverse_and_clear();
}

template <class PQ>
void diff::add(PQ& candidates, const sentence& sent)
{
    seen_.insert(sent.to_string());
    auto score = lambda_ * lm_.perplexity_per_word(sent)
                 + (1.0 - lambda_) * sent.average_weight();
    candidates.emplace(sent, score);
}

uint64_t diff::least_likely_ngram(const sentence& sent) const
{
    double min_prob = 1;
    uint64_t best_idx = 0;
    for (uint64_t i = n_val_; i < sent.size(); ++i)
    {
        auto ngram = sent(i - n_val_, i);
        auto prob = lm_.log_prob(ngram);
        if (prob < min_prob)
        {
            min_prob = prob;
            best_idx = i - 1;
        }
    }

    return best_idx;
}

template <class PQ>
void diff::lm_ops(const sentence& sent, PQ& candidates, uint64_t depth)
{
    auto best_idx = least_likely_ngram(sent);

    for (uint64_t i = 0; i < n_val_ && i < best_idx; ++i)
    {
        insert(sent, best_idx - i, candidates, depth);
        remove(sent, best_idx - i, candidates, depth);
        substitute(sent, best_idx - i, candidates, depth);
    }

    if (lm_generate_)
    {
        auto best = sent(best_idx - n_val_, best_idx);
        best.pop_back();
        try
        {
            for (auto& next : lm_.top_k(best, 3))
            {
                if (next.first == "</s>")
                    continue;

                sentence ins_cpy{sent};
                ins_cpy.insert(best_idx, next.first,
                               base_penalty_ + insert_penalty_);

                if (seen_.find(ins_cpy.to_string()) == seen_.end())
                {
                    add(candidates, ins_cpy);
                    step(ins_cpy, candidates, depth + 1);
                }

                sentence sub_cpy{sent};
                sub_cpy.substitute(best_idx, next.first,
                                   base_penalty_ + substitute_penalty_);

                if (seen_.find(sub_cpy.to_string()) == seen_.end())
                {
                    add(candidates, sub_cpy);
                    step(sub_cpy, candidates, depth + 1);
                }
            }
        }
        catch (language_model_exception& ex)
        {
            // ignore if there are no transitions found
        }
    }
}

template <class PQ>
void diff::insert(const sentence& sent, size_t idx, PQ& candidates,
                  uint64_t depth)
{
    for (const auto& fw : fwords_)
    {
        sentence ins_cpy{sent};
        ins_cpy.insert(idx, fw, base_penalty_ + insert_penalty_);
        if (seen_.find(ins_cpy.to_string()) == seen_.end())
        {
            add(candidates, ins_cpy);
            step(ins_cpy, candidates, depth + 1);
        }
    }
}

template <class PQ>
void diff::substitute(const sentence& sent, size_t idx, PQ& candidates,
                      uint64_t depth)
{
    std::string stemmed{sent[idx]};
    Porter2Stemmer::stem(stemmed);
    auto it = stems_.find(stemmed);
    if (it != stems_.end())
    {
        for (auto& stem : it->second)
        {
            // don't replace with same word!
            if (sent[idx] == stem)
                continue;
            sentence subbed{sent};
            subbed.substitute(idx, stem, base_penalty_ + substitute_penalty_);
            if (seen_.find(subbed.to_string()) == seen_.end())
            {
                add(candidates, subbed);
                step(subbed, candidates, depth + 1);
            }
        }
    }
}

template <class PQ>
void diff::remove(const sentence& sent, size_t idx, PQ& candidates,
                  uint64_t depth)
{
    sentence rem_cpy{sent};
    rem_cpy.remove(idx, base_penalty_ + remove_penalty_);
    if (seen_.find(rem_cpy.to_string()) == seen_.end())
    {
        add(candidates, rem_cpy);
        step(rem_cpy, candidates, depth + 1);
    }
}

template <class PQ>
void diff::step(const sentence& sent, PQ& candidates, size_t depth)
{
    if (depth == max_edits_)
        return;

    if (use_lm_)
        lm_ops(sent, candidates, depth);
    else
    {
        for (size_t i = 0; i < sent.size(); ++i)
        {
            remove(sent, i, candidates, depth);
            insert(sent, i, candidates, depth);
            substitute(sent, i, candidates, depth);
        }
    }
}

void diff::set_function_words(const cpptoml::table& config)
{
    std::ifstream in{*config.get_as<std::string>("function-words")};
    std::string word;
    while (in >> word)
        fwords_.push_back(word);
}

void diff::set_stems(const cpptoml::table& config)
{
    std::unordered_set<std::string> vocab;
    auto prefix = *config.get_as<std::string>("prefix");
    auto dataset = *config.get_as<std::string>("dataset");
    std::ifstream in{prefix + "/" + dataset + "/" + dataset + ".dat"};
    std::string token;
    while (in >> token)
        vocab.insert(utf::foldcase(token));

    for (const auto& t : vocab)
    {
        std::string stemmed{t};
        Porter2Stemmer::stem(stemmed);
        stems_[stemmed].push_back(t);
    }
}
}
}
