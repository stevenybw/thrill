/*******************************************************************************
 * examples/page_rank/page_rank_run.cpp
 *
 * Part of Project Thrill - http://project-thrill.org
 *
 * Copyright (C) 2016 Timo Bingmann <tb@panthema.net>
 * Copyright (C) 2016 Alexander Noe <aleexnoe@gmail.com>
 *
 * All rights reserved. Published under the BSD-2 license in the LICENSE file.
 ******************************************************************************/

#include <examples/page_rank/page_rank.hpp>
#include <examples/page_rank/zipf_graph_gen.hpp>

#include <thrill/api/cache.hpp>
#include <thrill/api/group_by_key.hpp>
#include <thrill/api/group_to_index.hpp>
#include <thrill/api/max.hpp>
#include <thrill/api/read_lines.hpp>
#include <thrill/api/sum.hpp>
#include <thrill/api/write_lines.hpp>
#include <thrill/api/zip_with_index.hpp>
#include <thrill/common/logger.hpp>
#include <thrill/common/stats_timer.hpp>
#include <tlx/cmdline_parser.hpp>

#include <algorithm>
#include <string>
#include <utility>
#include <vector>

using namespace thrill;              // NOLINT
using namespace examples::page_rank; // NOLINT

struct PageRankLineParser {
    PagePageLink operator () (const std::string& input) const {
        // parse "source\ttarget\n" lines
        char* endptr;
        unsigned long src = std::strtoul(input.c_str(), &endptr, 10);
        die_unless(endptr && *endptr == ' ' &&
                   "Could not parse src tgt line");
        unsigned long tgt = std::strtoul(endptr + 1, &endptr, 10);
        die_unless(endptr && *endptr == 0 &&
                   "Could not parse src tgt line");
        return PagePageLink { src, tgt };
    }
};

static void RunPageRankEdgePerLine(
    api::Context& ctx,
    const std::vector<std::string>& input_path, const std::string& output_path,
    size_t iterations) {
    ctx.enable_consume();

    common::StatsTimerStart timer;

    // read input file and create links in this format:
    //
    // url linked_url
    // url linked_url
    // url linked_url
    // ...
    auto input =
        ReadLines(ctx, input_path)
        .Map(PageRankLineParser());

    size_t num_pages =
        input.Keep()
        .Map([](const PagePageLink& ppl) { return std::max(ppl.src, ppl.tgt); })
        .Max() + 1;

    auto number_edges_future = input.Keep().SizeFuture();

    // aggregate all outgoing links of a page in this format: by index
    // ([linked_url, linked_url, ...])

    // group outgoing links from input file

    auto links = input.template GroupToIndex<OutgoingLinks>(
        [](const PagePageLink& p) { return p.src; },
        [all = std::vector<PageId> ()](auto& r, const PageId&) mutable {
            all.clear();
            while (r.HasNext()) {
                all.push_back(r.Next().tgt);
            }
            return all;
        },
        num_pages).Cache();

    // perform actual page rank calculation iterations

    auto ranks = PageRank(links, num_pages, iterations);

    // construct output as "pageid: rank"

    if (output_path.size()) {
        ranks.ZipWithIndex(
            // generate index numbers: 0...num_pages-1
            [](const Rank& r, const PageId& p) {
                return tlx::ssprintf("%zu: %g", p, r);
            })
        .WriteLines(output_path);
    }
    else {
        ranks.Execute();
    }

    timer.Stop();

    // SizeFuture must be read by all workers.
    size_t number_edges = number_edges_future();

    if (ctx.my_rank() == 0) {
        LOG1 << "FINISHED PAGERANK COMPUTATION";
        LOG1 << "#pages: " << num_pages;
        LOG1 << "#edges: " << number_edges;
        LOG1 << "#iterations: " << iterations;
        LOG1 << "time: " << timer << "s";
    }
}

static void RunJoinPageRankEdgePerLine(
    api::Context& ctx,
    const std::vector<std::string>& input_path, const std::string& output_path,
    size_t iterations) {
    ctx.enable_consume();

    common::StatsTimerStart timer;

    const bool UseLocationDetection = true;

    // read input file and create links in this format:
    //
    // url linked_url
    // url linked_url
    // url linked_url
    // ...
    auto input =
        ReadLines(ctx, input_path)
        .Map(PageRankLineParser());

    // aggregate all outgoing links of a page in this format: by index
    // ([linked_url, linked_url, ...])

    // group outgoing links from input file

    auto links = input.GroupByKey<LinkedPage>(
        [](const PagePageLink& p) { return p.src; },
        [all = std::vector<PageId> ()](auto& r, const PageId& pid) mutable {
            all.clear();
            while (r.HasNext()) {
                all.push_back(r.Next().tgt);
            }
            return std::make_pair(pid, all);
        }).Cache().KeepForever();

    // perform actual page rank calculation iterations

    auto ranks = PageRankJoinSelf<UseLocationDetection>(
        links, iterations);

    // construct output as "pageid: rank"

    if (output_path.size()) {
        ranks.Map([](const RankedPage& rp) {
                      return tlx::ssprintf("%zu: %g", rp.first, rp.second);
                  }).WriteLines(output_path);
    }
    else {
        ranks.Execute();
    }

    timer.Stop();

    if (ctx.my_rank() == 0) {
        if (UseLocationDetection) {
            LOG1 << "RESULT benchmark=pagerank_gen detection=ON"
                 << " iterations=" << iterations
                 << " time=" << timer
                 << " traffic= " << ctx.net_manager().Traffic()
                 << " hosts=" << ctx.num_hosts();
        }
        else {
            LOG1 << "RESULT benchmark=pagerank_gen detection=OFF"
                 << " iterations=" << iterations
                 << " time=" << timer
                 << " traffic=" << ctx.net_manager().Traffic()
                 << " hosts=" << ctx.num_hosts();
        }
    }
}

static void RunPageRankGenerated(
    api::Context& ctx,
    const std::string& input_path, const ZipfGraphGen& base_graph_gen,
    const std::string& output_path, size_t iterations) {
    ctx.enable_consume();

    common::StatsTimerStart timer;

    size_t num_pages;
    if (!common::from_str<size_t>(input_path, num_pages))
        die("For generated graph data, set input_path to the number of pages.");

    auto links = Generate(
        ctx, num_pages,
        [graph_gen = ZipfGraphGen(base_graph_gen, num_pages),
         rng = std::default_random_engine(std::random_device { } ())](
            size_t /* index */) mutable {
            return graph_gen.GenerateOutgoing(rng);
        })
                 .Cache();

    auto number_edges =
        links.Keep().Map([](const OutgoingLinks& ol) { return ol.size(); }).Sum();

    // perform actual page rank calculation iterations

    auto ranks = PageRank(links, num_pages, iterations);

    // construct output as "pageid: rank"

    if (output_path.size()) {
        ranks.ZipWithIndex(
            // generate index numbers: 0...num_pages-1
            [](const PageId& p, const Rank& r) {
                return std::to_string(p) + ": " + std::to_string(r);
            })
        .WriteLines(output_path);
    }
    else {
        ranks.Execute();
    }

    timer.Stop();

    if (ctx.my_rank() == 0) {
        LOG1 << "RESULT"
             << " benchmark=pagerank_gen"
             << " pages=" << num_pages
             << " edges=" << number_edges
             << " iterations=" << iterations
             << " time=" << timer
             << " hosts=" << ctx.num_hosts();
    }
}

static void RunPageRankJoinGenerated(
    api::Context& ctx,
    const std::string& input_path, const ZipfGraphGen& base_graph_gen,
    const std::string& output_path, size_t iterations) {
    ctx.enable_consume();

    common::StatsTimerStart timer;
    const bool UseLocationDetection = true;

    size_t num_pages;
    if (!common::from_str<size_t>(input_path, num_pages))
        die("For generated graph data, set input_path to the number of pages.");

    auto links = Generate(
        ctx, num_pages,
        [graph_gen = ZipfGraphGen(base_graph_gen, num_pages),
         rng = std::default_random_engine(std::random_device { } ())](
            size_t index) mutable {
            return std::make_pair(index, graph_gen.GenerateOutgoing(rng));
        }).Cache().KeepForever();

    // perform actual page rank calculation iterations

    auto ranks = PageRankJoin<UseLocationDetection>(
        links, num_pages, iterations);

    // construct output as "pageid: rank"

    if (output_path.size()) {
        ranks.Map([](const RankedPage& rp) {
                      return tlx::ssprintf("%zu: %g", rp.first, rp.second);
                  }).WriteLines(output_path);
    }
    else {
        ranks.Execute();
    }

    timer.Stop();

    if (ctx.my_rank() == 0) {
        if (UseLocationDetection) {
            LOG1 << "RESULT benchmark=pagerank_gen detection=ON"
                 << " pages=" << num_pages
                 << " time=" << timer
                 << " traffic= " << ctx.net_manager().Traffic()
                 << " hosts=" << ctx.num_hosts();
        }
        else {
            LOG1 << "RESULT benchmark=pagerank_gen detection=OFF"
                 << " pages=" << num_pages
                 << " time=" << timer
                 << " traffic=" << ctx.net_manager().Traffic()
                 << " hosts=" << ctx.num_hosts();
        }
    }
}

int main(int argc, char* argv[]) {

    tlx::CmdlineParser clp;

    bool generate = false;
    clp.add_bool('g', "generate", generate,
                 "generate graph data, set input = #pages");
    bool use_join = false;
    clp.add_bool('j', "join", use_join,
                 "use Join() instead of *ByIndex()");

    // Graph Generator
    ZipfGraphGen gg(1);

    clp.add_double(0, "size_mean", gg.size_mean,
                   "generated: mean of number of outgoing links, "
                   "default: " + std::to_string(gg.size_mean));

    clp.add_double(0, "size_var", gg.size_var,
                   "generated: variance of number of outgoing links, "
                   "default: " + std::to_string(gg.size_var));

    clp.add_double(0, "link_scale", gg.link_zipf_scale,
                   "generated: Zipf scale parameter for outgoing links, "
                   "default: " + std::to_string(gg.link_zipf_scale));

    clp.add_double(0, "link_exponent", gg.link_zipf_exponent,
                   "generated: Zipf exponent parameter for outgoing links, "
                   "default: " + std::to_string(gg.link_zipf_exponent));

    std::string output_path;
    clp.add_string('o', "output", output_path,
                   "output file pattern");

    size_t iter = 10;
    clp.add_size_t('n', "iterations", iter, "PageRank iterations, default: 10");

    std::vector<std::string> input_path;
    clp.add_param_stringlist("input", input_path,
                             "input file pattern(s)");

    if (!clp.process(argc, argv)) {
        return -1;
    }

    clp.print_result();

    die_unless(!generate || input_path.size() == 1);

    return api::Run(
        [&](api::Context& ctx) {
            if (generate && !use_join)
                return RunPageRankGenerated(
                    ctx, input_path[0], gg, output_path, iter);
            else if (!generate && !use_join)
                return RunPageRankEdgePerLine(
                    ctx, input_path, output_path, iter);
            else if (generate && use_join)
                return RunPageRankJoinGenerated(
                    ctx, input_path[0], gg, output_path, iter);
            else if (!generate && use_join)
                return RunJoinPageRankEdgePerLine(
                    ctx, input_path, output_path, iter);
        });
}

/******************************************************************************/
