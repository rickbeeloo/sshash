#pragma once

#include "../gz/zip_stream.hpp"

namespace sshash {

struct parse_data {
    parse_data(std::string const& tmp_dirname) : num_kmers(0), minimizers(tmp_dirname) {}
    uint64_t num_kmers;
    minimizers_tuples minimizers;
    compact_string_pool strings;
    weights::builder weights_builder;
};

void parse_file(std::istream& is, parse_data& data, build_configuration const& build_config) {
    const uint64_t k = build_config.k;
    const uint64_t m = build_config.m;
    const uint64_t seed = build_config.seed;
    const uint64_t max_num_kmers_in_super_kmer = k - m + 1;

    if (max_num_kmers_in_super_kmer >= (1ULL << (sizeof(num_kmers_in_super_kmer_uint_type) * 8))) {
        throw std::runtime_error(
            "max_num_kmers_in_super_kmer " + std::to_string(max_num_kmers_in_super_kmer) +
            " does not fit into " + std::to_string(sizeof(num_kmers_in_super_kmer_uint_type) * 8) +
            " bits");
    }

    /* fit into the wanted number of bits */
    assert(max_num_kmers_in_super_kmer < (1ULL << (sizeof(num_kmers_in_super_kmer_uint_type) * 8)));

    compact_string_pool::builder builder(k);

    std::string sequence;
    uint64_t prev_minimizer = constants::invalid_uint64;

    uint64_t begin = 0;  // begin of parsed super_kmer in sequence
    uint64_t end = 0;    // end of parsed super_kmer in sequence
    uint64_t num_sequences = 0;
    uint64_t num_bases = 0;
    bool glue = false;

    auto append_super_kmer = [&]() {
        if (sequence.empty() or prev_minimizer == constants::invalid_uint64 or begin == end) {
            return;
        }
        assert(end > begin);
        char const* super_kmer = sequence.data() + begin;
        uint64_t size = (end - begin) + k - 1;
        assert(util::is_valid(super_kmer, size));
        uint64_t num_kmers_in_super_kmer = end - begin;
        assert(num_kmers_in_super_kmer <= max_num_kmers_in_super_kmer);
        data.minimizers.emplace_back(prev_minimizer, builder.offset, num_kmers_in_super_kmer);
        builder.append(super_kmer, size, glue);
        if (glue) {
            assert(data.minimizers.back().offset > k - 1);
            data.minimizers.back().offset -= k - 1;
        }
        glue = true;
    };

    uint64_t seq_len = 0;
    uint64_t sum_of_weights = 0;
    data.weights_builder.init();

    /* intervals of weights */
    uint64_t weight_value = constants::invalid_uint64;
    uint64_t weight_length = 0;

    auto parse_header = [&]() {
        if (sequence.empty()) return;

        /*
            Heder format:
            >[id] LN:i:[seq_len] ab:Z:[weight_seq]
            where [weight_seq] is a space-separated sequence of integer counters (the weights),
            whose length is equal to [seq_len]-k+1
        */

        // example header: '>12 LN:i:41 ab:Z:2 2 2 2 2 2 2 2 2 2 2'

        expect(sequence[0], '>');
        uint64_t i = 0;
        i = sequence.find_first_of(' ', i);
        if (i == std::string::npos) throw parse_runtime_error();

        i += 1;
        expect(sequence[i + 0], 'L');
        expect(sequence[i + 1], 'N');
        expect(sequence[i + 2], ':');
        expect(sequence[i + 3], 'i');
        expect(sequence[i + 4], ':');
        i += 5;
        uint64_t j = sequence.find_first_of(' ', i);
        if (j == std::string::npos) throw parse_runtime_error();

        seq_len = std::strtoull(sequence.data() + i, nullptr, 10);
        i = j + 1;
        expect(sequence[i + 0], 'a');
        expect(sequence[i + 1], 'b');
        expect(sequence[i + 2], ':');
        expect(sequence[i + 3], 'Z');
        expect(sequence[i + 4], ':');
        i += 5;

        for (uint64_t j = 0; j != seq_len - k + 1; ++j) {
            uint64_t weight = std::strtoull(sequence.data() + i, nullptr, 10);
            i = sequence.find_first_of(' ', i) + 1;

            data.weights_builder.eat(weight);
            sum_of_weights += weight;

            if (weight == weight_value) {
                weight_length += 1;
            } else {
                if (weight_value != constants::invalid_uint64) {
                    data.weights_builder.push_weight_interval(weight_value, weight_length);
                }
                weight_value = weight;
                weight_length = 1;
            }
        }
    };

    while (!is.eof()) {
        std::getline(is, sequence);  // header sequence
        if (build_config.weighted) parse_header();

        std::getline(is, sequence);  // DNA sequence
        if (sequence.size() < k) continue;

        if (++num_sequences % 100000 == 0) {
            std::cout << "read " << num_sequences << " sequences, " << num_bases << " bases, "
                      << data.num_kmers << " kmers" << std::endl;
        }

        begin = 0;
        end = 0;
        glue = false;  // start a new piece
        prev_minimizer = constants::invalid_uint64;
        uint64_t prev_pos = constants::invalid_uint64;
        num_bases += sequence.size();

        if (build_config.weighted and seq_len != sequence.size()) {
            std::cout << "ERROR: expected a sequence of length " << seq_len
                      << " but got one of length " << sequence.size() << std::endl;
            throw std::runtime_error("file is malformed");
        }

        while (end != sequence.size() - k + 1) {
            char const* kmer = sequence.data() + end;
            assert(util::is_valid(kmer, k));
            kmer_t uint_kmer = util::string_to_uint_kmer_no_reverse(kmer, k);
            auto [minimizer, pos] = util::compute_minimizer_pos(uint_kmer, k, m, seed);

            if (build_config.canonical_parsing) {
                kmer_t uint_kmer_rc = util::compute_reverse_complement(uint_kmer, k);
                auto [minimizer_rc, pos_rc] = util::compute_minimizer_pos(uint_kmer_rc, k, m, seed);
                if (minimizer_rc < minimizer) {
                    minimizer = minimizer_rc;
                    assert(k >= pos_rc + m);
                    pos = k - (pos_rc + m);
                }
            }

            if (prev_minimizer == constants::invalid_uint64) {
                prev_minimizer = minimizer;
                prev_pos = pos + 1;
            }
            if (minimizer != prev_minimizer or pos + 1 != prev_pos) {
                append_super_kmer();
                begin = end;
                prev_minimizer = minimizer;
                glue = true;
            }

            ++data.num_kmers;
            ++end;
            prev_pos = pos;
        }

        append_super_kmer();
    }

    data.minimizers.finalize();
    builder.finalize();
    builder.build(data.strings);

    std::cout << "read " << num_sequences << " sequences, " << num_bases << " bases, "
              << data.num_kmers << " kmers" << std::endl;
    std::cout << "num_kmers " << data.num_kmers << std::endl;
    std::cout << "num_super_kmers " << data.strings.num_super_kmers() << std::endl;
    std::cout << "num_pieces " << data.strings.pieces.size() << " (+"
              << (2.0 * data.strings.pieces.size() * (k - 1)) / data.num_kmers << " [bits/kmer])"
              << std::endl;
    assert(data.strings.pieces.size() == num_sequences + 1);

    if (build_config.weighted) {
        std::cout << "sum_of_weights " << sum_of_weights << std::endl;
        data.weights_builder.push_weight_interval(weight_value, weight_length);
        data.weights_builder.finalize(data.num_kmers);
    }
}

parse_data parse_file(std::string const& filename, build_configuration const& build_config) {
    std::ifstream is(filename.c_str());
    if (!is.good()) throw std::runtime_error("error in opening the file '" + filename + "'");
    std::cout << "reading file '" << filename << "'..." << std::endl;
    parse_data data(build_config.tmp_dirname);
    if (util::ends_with(filename, ".gz")) {
        zip_istream zis(is);
        parse_file(zis, data, build_config);
    } else {
        parse_file(is, data, build_config);
    }
    is.close();
    return data;
}

}  // namespace sshash