/*******************************************************************************
 * examples/page_rank/page_rank.hpp
 *
 * Part of Project Thrill - http://project-thrill.org
 *
 * Copyright (C) 2016 Timo Bingmann <tb@panthema.net>
 * Copyright (C) 2016 Alexander Noe <aleexnoe@gmail.com>
 *
 * All rights reserved. Published under the BSD-2 license in the LICENSE file.
 ******************************************************************************/

#pragma once
#ifndef THRILL_EXAMPLES_PAGE_RANK_PAGE_RANK_HEADER
#define THRILL_EXAMPLES_PAGE_RANK_PAGE_RANK_HEADER

#include <thrill/api/collapse.hpp>
#include <thrill/api/generate.hpp>
#include <thrill/api/inner_join.hpp>
#include <thrill/api/print.hpp>
#include <thrill/api/reduce_by_key.hpp>
#include <thrill/api/reduce_to_index.hpp>
#include <thrill/api/size.hpp>
#include <thrill/api/zip.hpp>
#include <thrill/common/logger.hpp>

#include <tlx/string/join_generic.hpp>

#include <algorithm>
#include <string>
#include <utility>
#include <vector>
#include <chrono>

namespace examples {
namespace page_rank {

using namespace thrill; // NOLINT

static constexpr bool debug = false;

static constexpr double dampening = 0.85;

using PageId = std::size_t;
using Rank = double;

//! A pair (page source, page target)
struct PagePageLink {
    PageId src, tgt;

    friend std::ostream& operator << (std::ostream& os, const PagePageLink& a) {
        return os << '(' << a.src << '>' << a.tgt << ')';
    }
} TLX_ATTRIBUTE_PACKED;

//! A pair (page, rank)
struct PageRankPair {
    PageId page;
    Rank   rank;

    friend std::ostream& operator << (std::ostream& os, const PageRankPair& a) {
        return os << '(' << a.page << '|' << a.rank << ')';
    }
} TLX_ATTRIBUTE_PACKED;

using PageRankStdPair = std::pair<PageId, Rank>;
using OutgoingLinks = std::vector<PageId>;
using OutgoingLinksRank = std::pair<std::vector<PageId>, Rank>;
using LinkedPage = std::pair<PageId, OutgoingLinks>;
using RankedPage = std::pair<PageId, Rank>;

template <typename InStack>
auto PageRank(const DIA<OutgoingLinks, InStack>& links,
              size_t num_pages, size_t iterations) {

    api::Context& ctx = links.context();
    double num_pages_d = static_cast<double>(num_pages);

    // initialize all ranks to 1.0 / n: (url, rank)

    DIA<Rank> ranks =
        Generate(
            ctx, num_pages,
            [num_pages_d](size_t) { return Rank(1.0) / num_pages_d; })
        .Collapse();

    // do iterations
    for (size_t iter = 0; iter < iterations; ++iter) {

        // for all outgoing link, get their rank contribution from all
        // links by doing:
        //
        // 1) group all outgoing links with rank of its parent page: (Zip)
        // ([linked_url, linked_url, ...], rank_parent)
        //
        // 2) compute rank contribution for each linked_url: (FlatMap)
        // (linked_url, rank / outgoing.size)

        auto outs_rank = links.Zip(
            ranks,
            [](const OutgoingLinks& ol, const Rank& r) {
                return OutgoingLinksRank(ol, r);
            });

        if (debug) {
            outs_rank
            .Map([](const OutgoingLinksRank& ol) {
                     return tlx::join(',', ol.first)
                     + " <- " + std::to_string(ol.second);
                 })
            .Print("outs_rank");
        }

        auto contribs = outs_rank.template FlatMap<PageRankPair>(
            [](const OutgoingLinksRank& p, auto emit) {
                if (p.first.size() == 0)
                    return;

                Rank rank_contrib = p.second / static_cast<double>(p.first.size());
                for (const PageId& tgt : p.first)
                    emit(PageRankPair { tgt, rank_contrib });
            });

        // reduce all rank contributions by adding all rank contributions and
        // compute the new rank: (url, rank)

        ranks =
            contribs
            .ReduceToIndex(
                [](const PageRankPair& p) { return p.page; },
                [](const PageRankPair& p1, const PageRankPair& p2) {
                    return PageRankPair { p1.page, p1.rank + p2.rank };
                }, num_pages)
            .Map([num_pages_d](const PageRankPair& p) {
                     return dampening * p.rank + (1 - dampening) / num_pages_d;
                 })
            .Collapse();
    }

    return ranks;
}

template <const bool UseLocationDetection = true, typename InStack>
auto PageRankJoinSelf(const DIA<LinkedPage, InStack>& links, size_t iterations) {

    api::Context& ctx = links.context();
    // initialize all ranks to 1.0 / n: (url, rank)

    DIA<RankedPage> ranks = links.Map([](const LinkedPage &lp) {
        return std::make_pair(lp.first, Rank(1.0));
    });

    // do iterations
    auto time_start = std::chrono::high_resolution_clock::now();
    for (size_t iter = 0; iter < iterations; ++iter) {

        // for all outgoing link, get their rank contribution from all
        // links by doing:
        //
        // 1) group all outgoing links with rank of its parent page: (Zip)
        // ([linked_url, linked_url, ...], rank_parent)
        //
        // 2) compute rank contribution for each linked_url: (FlatMap)
        // (linked_url, rank / outgoing.size)
        if (debug && iter == 0) {
            links
            .Map([](const LinkedPage& ol) {
                     return tlx::join(',', ol.second)
                     + " <- " + std::to_string(ol.first);
                 })
            .Print("outs_rank");
        }

        auto outs_rank = InnerJoin(
            LocationDetectionFlag<UseLocationDetection>(),
            links, ranks,
            [](const LinkedPage& lp) { return lp.first; },
            [](const RankedPage& rp) { return rp.first; },
            [](const LinkedPage& lp, const RankedPage& rp) {
                return std::make_pair(lp.second, rp.second);
            });

        //if (debug && iter == 1) {
        //    outs_rank
        //    .Map([](const OutgoingLinksRank& ol) {
        //             return tlx::join(',', ol.first)
        //             + " <- " + std::to_string(ol.second);
        //         })
        //    .Print("outs_rank");
        //}

        auto contribs = outs_rank.template FlatMap<PageRankStdPair>(
            [](const OutgoingLinksRank& p, auto emit) {
                if (p.first.size() > 0) {
                    Rank rank_contrib = p.second / static_cast<double>(p.first.size());
                    for (const PageId& tgt : p.first)
                        emit(std::make_pair(tgt, rank_contrib));
                }
            });

        // reduce all rank contributions by adding all rank contributions and
        // compute the new rank: (url, rank)
        ranks =
            contribs
            .ReducePair(
                [](const Rank& p1, const Rank& p2) {
                    return p1 + p2;
                })
            .Map([](const PageRankStdPair& p) {
                     return std::make_pair(
                         p.first,
                         dampening * p.second + (1 - dampening));
                 }).Collapse();

        ranks.Execute();
        auto time_now = std::chrono::high_resolution_clock::now();
        std::chrono::duration<double> diff = time_now - time_start;
        if (ctx.my_rank() == 0) {
            std::cout << "step " << iter << ", time: " << diff.count() << " s" << std::endl;
        }
    }
    return ranks;
}

template <const bool UseLocationDetection = false, typename InStack>
auto PageRankJoin(const DIA<LinkedPage, InStack>& links, size_t num_pages,
                  size_t iterations) {

    api::Context& ctx = links.context();
    double num_pages_d = static_cast<double>(num_pages);

    // initialize all ranks to 1.0 / n: (url, rank)

    DIA<RankedPage> ranks =
        Generate(
            ctx, num_pages,
            [num_pages_d](size_t idx) {
                return std::make_pair(idx, Rank(1.0) / num_pages_d);
            })
        .Collapse();

    // do iterations
    for (size_t iter = 0; iter < iterations; ++iter) {

        // for all outgoing link, get their rank contribution from all
        // links by doing:
        //
        // 1) group all outgoing links with rank of its parent page: (Zip)
        // ([linked_url, linked_url, ...], rank_parent)
        //
        // 2) compute rank contribution for each linked_url: (FlatMap)
        // (linked_url, rank / outgoing.size)

        auto outs_rank = InnerJoin(
            LocationDetectionFlag<UseLocationDetection>(),
            links, ranks,
            [](const LinkedPage& lp) { return lp.first; },
            [](const RankedPage& rp) { return rp.first; },
            [](const LinkedPage& lp, const RankedPage& rp) {
                return std::make_pair(lp.second, rp.second);
            });

        if (debug) {
            outs_rank
            .Map([](const OutgoingLinksRank& ol) {
                     return tlx::join(',', ol.first)
                     + " <- " + std::to_string(ol.second);
                 })
            .Print("outs_rank");
        }

        auto contribs = outs_rank.template FlatMap<PageRankStdPair>(
            [](const OutgoingLinksRank& p, auto emit) {
                if (p.first.size() > 0) {
                    Rank rank_contrib = p.second / static_cast<double>(p.first.size());
                    for (const PageId& tgt : p.first)
                        emit(std::make_pair(tgt, rank_contrib));
                }
            });

        // reduce all rank contributions by adding all rank contributions and
        // compute the new rank: (url, rank)

        ranks =
            contribs
            .ReducePair(
                [](const Rank& p1, const Rank& p2) {
                    return p1 + p2;
                })
            .Map([num_pages_d](const PageRankStdPair& p) {
                     return std::make_pair(
                         p.first,
                         dampening * p.second + (1 - dampening) / num_pages_d);
                 }).Collapse();

        ranks.Execute();
    }

    return ranks;
}

} // namespace page_rank
} // namespace examples

#endif // !THRILL_EXAMPLES_PAGE_RANK_PAGE_RANK_HEADER

/******************************************************************************/
