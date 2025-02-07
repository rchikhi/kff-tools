#include <vector>
#include <string>
#include <cstring>
#include <cmath>
#include <algorithm>
#include <queue>
#include <cassert>

#include "encoding.hpp"
#include "sequences.hpp"
#include "compact.hpp"
#include "merge.hpp"


using namespace std;


Compact::Compact() {
	input_filename = "";
	output_filename = "";
	sorted = false;

	this->buffer_size = 1 << 10;
	this->next_free = 0;
	this->kmer_buffer = (uint8_t *)malloc(this->buffer_size);
	memset(this->kmer_buffer, 0, this->buffer_size);

	this->k = 0;
	this->m = 0;
	this->bytes_compacted = 0;
	this->offset_idx = 0;
}


Compact::~Compact() {
	free(this->kmer_buffer);
}


void Compact::cli_prepare(CLI::App * app) {
	this->subapp = app->add_subcommand("compact", "Read a kff file and try to compact the kmers from minimizer sections. The available ram must be sufficent to load a complete minimizer section into memory.");
	CLI::Option * input_option = subapp->add_option("-i, --infile", input_filename, "Input kff file to compact.");
	input_option->required();
	input_option->check(CLI::ExistingFile);
	CLI::Option * out_option = subapp->add_option("-o, --outfile", output_filename, "Kff to write (must be different from the input)");
	out_option->required();
	// subapp->add_flag("-s, --sorted", sorted, "The output compacted superkmers will be sorted to allow binary search. Sorted superkmer have a lower compaction ratio (ie will be less compacted).");
}


void Compact::exec() {
	Kff_file infile(input_filename, "r");
	Kff_file outfile(output_filename, "w");

	outfile.write_encoding(infile.encoding);
	
	outfile.set_uniqueness(infile.uniqueness);
	outfile.set_canonicity(infile.canonicity);
	
	// Metadata transfer
	uint8_t * metadata = new uint8_t[infile.metadata_size];
	infile.read_metadata(metadata);
	outfile.write_metadata(infile.metadata_size, metadata);
	delete[] metadata;

	bool first_warning = true;

	while (infile.tellp() != infile.end_position) {
		char section_type = infile.read_section_type();

		if (section_type == 'v') {
			Section_GV isgv(&infile);
			isgv.close();

			unordered_map<string, uint64_t> to_copy;
			for (auto & p : isgv.vars) {
				if (p.first != "first_index" and p.first != "footer_size") {
					to_copy[p.first] = p.second;
				}
			}

			if (to_copy.size() > 0) {
				Section_GV osgv(&outfile);
				for (auto & p : isgv.vars)
					osgv.write_var(p.first, p.second);
				osgv.close();
			}
		}
		else if (section_type == 'i') {
			Section_Index si(&infile);
			si.close();
		}
		else if (section_type == 'r') {
			if (first_warning) {
				first_warning = false;
				cerr << "WARNING: kff-tools has detected R sections inside of the file. The compact tool is only compacting kmers inside of M sections. The R sections are omitted." << endl;
			}

			Section_Raw sr(&infile);
			sr.close();
		}
		else if (section_type == 'm') {
			uint k = outfile.global_vars["k"];
			uint m = outfile.global_vars["m"];

			// Rewrite a value section if max is not sufficently large
			if (outfile.global_vars["max"] < k - m + 1) {
				unordered_map<string, uint64_t> values(outfile.global_vars);
				Section_GV sgv(&outfile);

				for (auto & p : values)
					if (p.first != "max")
						sgv.write_var(p.first, p.second);
				sgv.write_var("max", k - m + 1);

				sgv.close();
			}

			// Compact and save the kmers
			Section_Minimizer sm(&infile);
			this->compact_section(sm, outfile);
			sm.close();
		}
	}

	infile.close();
	outfile.close();
}

void Compact::compact_section(Section_Minimizer & ism, Kff_file & outfile) {
	// General variables
	uint k = outfile.global_vars["k"];
	uint m = outfile.global_vars["m"];
	uint data_size = outfile.global_vars["data_size"];

	this->k = k;
	this->m = m;
	this->data_size = data_size;
	this->bytes_compacted = (k - m + 3) / 4;
	uint64_t max_nucl = (k - m) + 1;
	this->mini_pos_size = (static_cast<uint>(ceil(log2(max_nucl))) + 7) / 8;
	this->offset_idx = (4 - ((k - m) % 4)) % 4;
	
	// 1 - Load the input section
	vector<vector<uint8_t *> > kmers_per_index = this->prepare_kmer_matrix(ism);
	
	// 2 - Compact kmers
	vector<vector<uint8_t *> > paths;
	if (this->sorted) {
		paths = this->sorted_assembly(kmers_per_index);
		exit(0);
	} else {
		vector<pair<uint8_t *, uint8_t *> > to_compact = this->greedy_assembly(kmers_per_index);
		paths = this->pairs_to_paths(to_compact);
	}

	Section_Minimizer osm(&outfile);
	osm.write_minimizer(ism.minimizer);
	this->write_paths(paths, osm, data_size);
	osm.close();
}


long Compact::add_kmer_to_buffer(const uint8_t * seq, const uint8_t * data, uint64_t mini_pos) {
	// Realloc if needed
	if (this->buffer_size - this->next_free < this->bytes_compacted + this->data_size + this->mini_pos_size) {
		this->kmer_buffer = (uint8_t *) realloc((void *)this->kmer_buffer, this->buffer_size*2);
		memset(this->kmer_buffer + this->buffer_size, 0, this->buffer_size);
		this->buffer_size *= 2;
	}
	long position = this->next_free;
	uint8_t * buffer = this->kmer_buffer + this->next_free;

	// Copy kmer sequence
	memcpy(buffer, seq, this->bytes_compacted);
	buffer += this->bytes_compacted;
	// Copy data array
	memcpy(buffer, data, this->data_size);
	buffer += this->data_size;
	// Write mini position
	for (int b=mini_pos_size-1 ; b>=0 ; b--) {
		*(buffer + b) = mini_pos & 0xFF;
		mini_pos >>= 8;
	}
	this->next_free += this->bytes_compacted + this->data_size + this->mini_pos_size;

	return position;
}

vector<vector<uint8_t *> > Compact::prepare_kmer_matrix(Section_Minimizer & sm) {
	vector<vector<long> > pos_matrix;
	pos_matrix.resize(sm.k - sm.m + 1);
	
	uint64_t max_nucl = sm.k + sm.max - 1;
	uint64_t max_seq_bytes = (max_nucl + 3) / 4;
	uint64_t kmer_bytes = (sm.k - sm.m + 3) / 4;
	uint64_t mini_pos_size = (static_cast<uint>(ceil(log2(max_nucl))) + 7) / 8;

	uint8_t * seq_buffer = new uint8_t[max_seq_bytes];
	uint8_t * data_buffer = new uint8_t[sm.data_size * sm.max];

	// 1 - Load the input section
	for (uint n=0 ; n<sm.nb_blocks ; n++) {
		uint64_t mini_pos = 0xFFFFFFFFFFFFFFFF;
		// Read sequence
		uint nb_kmers = sm.read_compacted_sequence_without_mini(
			seq_buffer, data_buffer, mini_pos);

		// Add kmer by index
		for (uint kmer_idx=0 ; kmer_idx<nb_kmers ; kmer_idx++) {
			uint kmer_pos = sm.k - (uint)sm.m - mini_pos + kmer_idx;

			// Realloc if needed
			if (this->buffer_size - this->next_free < kmer_bytes + sm.data_size + mini_pos_size) {
				this->kmer_buffer = (uint8_t *) realloc((void *)this->kmer_buffer, this->buffer_size*2);
				memset(this->kmer_buffer + this->buffer_size, 0, this->buffer_size);
				this->buffer_size *= 2;
			}

			// Copy kmer sequence
			subsequence(seq_buffer, sm.k - sm.m + nb_kmers - 1, this->kmer_buffer + next_free, kmer_idx, kmer_idx + sm.k - sm.m - 1);
			// Copy data array
			memcpy(this->kmer_buffer + next_free + kmer_bytes, data_buffer + kmer_idx * sm.data_size, sm.data_size);
			// Write mini position
			uint kmer_mini_pos = mini_pos - kmer_idx;
			for (int b=mini_pos_size-1 ; b>=0 ; b--) {
				*(this->kmer_buffer + next_free + kmer_bytes + sm.data_size + b) = kmer_mini_pos & 0xFF;
				kmer_mini_pos >>= 8;
			}
			// Update
			pos_matrix[kmer_pos].push_back(this->next_free);
			next_free += kmer_bytes + sm.data_size + mini_pos_size;
		}
	}

	delete[] seq_buffer;
	delete[] data_buffer;

	// Transform the position matrix into the kmer matrix
	vector<vector<uint8_t *> > kmer_matrix;
	for (vector<long> & positions : pos_matrix) {
		vector<uint8_t *> column(positions.size(), nullptr);
		uint idx = 0;
		for (long pos : positions)
			column[idx++] = this->kmer_buffer + pos;
		kmer_matrix.push_back(column);
	}

	return kmer_matrix;
}


uint Compact::mini_pos_from_buffer(const uint8_t * kmer) const {
	uint mini_pos = 0;

	for (uint i=0 ; i<this->mini_pos_size ; i++) {
		mini_pos <<= 8;
		mini_pos += kmer[this->bytes_compacted + this->data_size + i];
	}

	return mini_pos;
}


int Compact::interleaved_compare_kmers(const uint8_t * kmer1, const uint8_t * kmer2) const {
	const uint mini_pos1 = this->mini_pos_from_buffer(kmer1);
	const uint mini_pos2 = this->mini_pos_from_buffer(kmer2);

	assert(mini_pos1 == mini_pos2);

	const uint used_nucl = this->k - this->m;
	const uint offset_nucl = (4 - (used_nucl % 4)) % 4;
	const uint pref_nucl = mini_pos1;
	const uint pref_bytes = (offset_nucl + pref_nucl + 3) / 4;
	const uint suff_nucl = used_nucl - pref_nucl;
	const uint suff_bytes = (suff_nucl + 3) / 4;
	const uint total_bytes = (used_nucl + 3) / 4;

	// --- Prefix ---
	int last_prefix_divergence = -1;
	// Prepare masks
	const uint pref_start_mask = (1u << (8 - 2 * offset_nucl)) - 1;
	const uint pref_stop_mask = ~((1u << (2 * (suff_nucl % 4))) - 1);
	// Iterate over all bytes
	for (uint pref_byte=0 ; pref_byte<pref_bytes ; pref_byte++) {
		uint8_t byte1 = kmer1[pref_byte];
		uint8_t byte2 = kmer2[pref_byte];

		bool end_byte = pref_byte == pref_bytes-1;

		// Mask useless bits
		if (pref_byte == 0) {
			byte1 &= pref_start_mask;
			byte2 &= pref_start_mask;
		}
		if (end_byte) {
			byte1 &= pref_stop_mask;
			byte2 &= pref_stop_mask;	
		}

		// Compare
		uint8_t result = byte1 xor byte2;
		// Get the rightmost bit set to 0: ie the last difference between sequences
		for (uint8_t i=0 ; i<4 ; i++) {
			if ((not end_byte) or (end_byte and (i >= (suff_nucl % 4)))) {
				if ((result & (0b11 << (2 * i))) != 0) {
					last_prefix_divergence = pref_byte * 4 + 3 - i - offset_nucl;	
					break;
				}
			}
		}
	}


	// --- Suffix ---
	int first_suffix_divergence = suff_nucl;
	int current_divergence_idx = 0;
	const uint suff_first_byte = total_bytes - suff_bytes;
	
	// Iterate over all the bytes from the suffix
	for (uint suff_byte=suff_first_byte ; suff_byte<total_bytes and first_suffix_divergence==(int)suff_nucl ; suff_byte++) {
		// Extract and compare bytes
		uint8_t byte1 = kmer1[suff_byte];
		uint8_t byte2 = kmer2[suff_byte];
		uint8_t result = byte1 xor byte2;

		for (uint8_t i=4 ; i>0 ; i--) {

			// Skip the first nucleotides of the first byte
			if (suff_byte == suff_first_byte and i == 4) {
				i = ((suff_nucl - 1) % 4) + 1;
			}

			// Check for divergeance
			if ((result & (0b11 << (2 * (i-1)))) != 0) {
				first_suffix_divergence = current_divergence_idx;
			} else {
				current_divergence_idx += 1;
			}
		}
	}

	// In case of sequence similarity
	if (last_prefix_divergence == -1 and first_suffix_divergence == (int)suff_nucl) {
		return 0;
	}
	// Check the first suffix divergence
	else {
		// Compute the divergence posi
		uint pref_div_distance = pref_nucl - last_prefix_divergence - 1;
		uint nucl_pos = offset_nucl;

		if (pref_div_distance == pref_nucl) {
			nucl_pos += pref_nucl + first_suffix_divergence;
		} else if (first_suffix_divergence == (int)suff_nucl) {
			nucl_pos += last_prefix_divergence;
		}
		// First interleaved divergence in the prefix
		else if ((int)pref_div_distance < first_suffix_divergence) {
			nucl_pos += last_prefix_divergence;
		}
		// First interleaved divergence in the suffix
		else {
			nucl_pos += pref_nucl + first_suffix_divergence;
		}

		// Extract the divergent nucleotides
		uint byte_pos = nucl_pos / 4;
		uint nucl_shift = 2 * (3 - (nucl_pos % 4));
		uint nucl1 = (kmer1[byte_pos] >> nucl_shift) & 0b11;
		uint nucl2 = (kmer2[byte_pos] >> nucl_shift) & 0b11;

		// Compare
		if (nucl1 < nucl2)
			return -1;
		else
			return +1;
	}
}


void Compact::sort_matrix(vector<vector<uint8_t *> > & kmer_matrix) {
	// Sort by column
	for (uint i=0 ; i<kmer_matrix.size() ; i++) {

		// Comparison function (depends on minimizer position)
		auto comp_function = [this](const uint8_t * kmer1, const uint8_t * kmer2) {
			return this->interleaved_compare_kmers(kmer1, kmer2) < 0;
		};

		sort(kmer_matrix[i].begin(), kmer_matrix[i].end(), comp_function);
	}
}


vector<pair<uint8_t *, uint8_t *> > Compact::pair_kmers(const vector<uint8_t *> & column1, const vector<uint8_t *> & column2) const {
	const uint nb_nucl = k - m;

	vector<pair<uint8_t *, uint8_t *> > pairs;
	pairs.reserve(max(column1.size(), column2.size()));

	// Index the second column by their prefix hash
	unordered_map<uint64_t, vector<uint8_t *> > index;
	unordered_map<uint8_t *, bool> used;
	for (uint8_t * kmer : column2) {
		// Get the hash corresponding to the k-m-1 prefix
		uint64_t hash = subseq_to_uint(kmer, nb_nucl, 0, nb_nucl-2);

		if (index.find(hash) == index.end())
			index[hash] = vector<uint8_t *>();
		index[hash].push_back(kmer);
		used[kmer] = false;
	}

	// Looks for suffix matches of the first column
	for (uint8_t * kmer : column1) {
		// Get the hash corresponding to the k-m-1 suffix
		uint64_t hash = subseq_to_uint(kmer, nb_nucl, 1, nb_nucl-1);

		// Test for hash collision
		if (index.find(hash) != index.end()) {
			uint candidate_pos = 0;
			// Test each of the 
			for (uint8_t * candidate : index[hash]) {
				if (sequence_compare(
							candidate, nb_nucl, 0, nb_nucl-2,
							kmer, nb_nucl, 1, nb_nucl-1
						) == 0) {
					
					pairs.emplace_back(kmer, candidate);
					used[candidate] = true;
				}
				candidate_pos += 1;
			}
		}

	}

	// Add the right kmers that are not paired
	for (auto & entry : used) {
		if (not entry.second)
			pairs.emplace_back(nullptr, entry.first);
	}

	return pairs;
}


vector<pair<uint8_t *, uint8_t *> > Compact::colinear_chaining(const vector<pair<uint8_t *, uint8_t *> > & candidates) const {
	vector<pair<uint8_t *, uint8_t *> > predecessors;
	vector<pair<uint8_t *, uint8_t *> > subseq_index;

	// 0 - Sorting the candidates

	// uint longest = 0;
	// uint idx=0;
	// for (const pair<uint8_t *, uint8_t *> & candidate : candidates) {
		

	// 	idx += 1;
	// }
	
	vector<pair<uint8_t *, uint8_t *> > selected;

	return selected;
}


vector<vector<uint8_t *> > Compact::polish_sort(const vector<vector<pair<uint8_t *, uint8_t *> > > & colinear_chainings) const {

	cerr << "TODO polish_sort" << endl;
	exit(1);

	return vector<vector<uint8_t *> >();
}


vector<vector<uint8_t *> > Compact::sorted_assembly(vector<vector<uint8_t *> > & kmers) {

	// 1 - Sort Matrix per column
	this->sort_matrix(kmers);

	vector<vector<pair<uint8_t *, uint8_t *> > > kmer_pairs;

	// Init first column
	vector<pair<uint8_t *, uint8_t *> > first_kmers;
	for (uint8_t * kmer : kmers[0])
		first_kmers.emplace_back(nullptr, kmer);
	kmer_pairs.push_back(first_kmers);

	// Pair columns
	for (uint i=0 ; i<this->k-this->m ; i++) {
		// 2 - Find all the possible overlaps of kmers
		const vector<pair<uint8_t *, uint8_t *> > candidate_links = this->pair_kmers(kmers[i], kmers[i+1]);

		// 3 - Filter out kmer pairs that are not in optimal colinear chainings
		const vector<pair<uint8_t *, uint8_t *> > colinear_links = this->colinear_chaining(candidate_links);
		kmer_pairs.push_back(colinear_links);
	}

	// 4 - Finish the ordering by sorting skmers that could have been interchanged
	const vector<vector<uint8_t *> > skmers = polish_sort(kmer_pairs);

	return skmers;
}


vector<pair<uint8_t *, uint8_t *> > Compact::greedy_assembly(vector<vector<uint8_t *> > & kmers) {
	uint nb_nucl = k - m;
	vector<pair<uint8_t *, uint8_t *> > assembly;

	// Index kmers from the 0th set
	for (uint8_t * kmer : kmers[0]) {
		assembly.emplace_back(nullptr, kmer);
	}

	for (uint i=0 ; i<nb_nucl ; i++) {
		// Index kmers in ith set
		unordered_map<uint64_t, vector<uint8_t *> > index;
		
		for (uint8_t * kmer : kmers[i]) {
			// Get the suffix
			uint64_t val = subseq_to_uint(kmer, nb_nucl, 1, nb_nucl-1);
			// Add a new vector for this value
			if (index.find(val) == index.end())
				index[val] = vector<uint8_t *>();
			// Add the kmer to the value list
			index[val].push_back(kmer);
		}

		// link kmers from (i+1)th set to ith kmers.
		for (uint8_t * kmer : kmers[i+1]) {
			uint64_t val = subseq_to_uint(kmer, nb_nucl, 0, nb_nucl-2);

			if (index.find(val) == index.end()) {
				// No kmer available for matching
				assembly.emplace_back(nullptr, kmer);
			} else {
				bool chaining_found = false;
				uint candidate_pos = 0;
				// verify complete matching for candidates kmers
				for (uint8_t * candidate : index[val]) {
					// If the kmers can be assembled
					if (sequence_compare(
								kmer, nb_nucl, 0, nb_nucl-2,
								candidate, nb_nucl, 1, nb_nucl-1
							) == 0) {
						// Update status
						chaining_found = true;
						assembly.emplace_back(candidate, kmer);

						// remove candidate from list
						index[val].erase(index[val].begin()+candidate_pos);
						// Quit candidate searching
						break;
					}

					candidate_pos += 1;
				}
				// If no assembly possible, create a new superkmer
				if (not chaining_found) {
					assembly.emplace_back(nullptr, kmer);
				}
			}
		}
	}

	// Index last kmers without compaction
	int assembly_idx = assembly.size()-1;
	for (auto it=kmers[nb_nucl].end() ; it>kmers[nb_nucl].begin() ; it--) {
		uint8_t * kmer = *(it-1);

		if (assembly_idx < 0 or kmer != assembly[assembly_idx].second) {
			assembly.emplace_back(nullptr, kmer);
		} else {
			assembly_idx--;
		}
	}

	return assembly;
}

vector<vector<uint8_t *> > Compact::pairs_to_paths(const vector<pair<uint8_t *, uint8_t *> > & to_compact) {
	vector<vector<uint8_t *> > paths;
	unordered_map<uint8_t *, uint> path_registry;

	for (const pair<uint8_t *, uint8_t *> & p : to_compact) {
		// First element of a compaction path
		if (p.first == nullptr) {
			path_registry[p.second] = paths.size();
			paths.emplace_back(vector<uint8_t *>());

			uint vec_idx = path_registry[p.second];
			paths[vec_idx].push_back(p.second);
		}
		// Extending existing path
		else {
			uint vec_idx = path_registry[p.first];
			paths[vec_idx].push_back(p.second);
			path_registry.erase(p.first);
			path_registry[p.second] = vec_idx;
		}
	}

	return paths;
}

void Compact::write_paths(const vector<vector<uint8_t *> > & paths, Section_Minimizer & sm, const uint data_size) {
	uint kmer_bytes = (k - m + 3) / 4;
	uint kmer_offset = (4 - ((k - m) % 4)) % 4;
	uint mini_pos_size = (static_cast<uint>(ceil(log2(sm.max + k - m))) + 7) / 8;

	uint max_skmer_bytes = (2 * (k - m) + 3) / 4;
	uint8_t * skmer_buffer = new uint8_t[max_skmer_bytes + 1];
	uint data_bytes = (k - m + 1) * data_size;
	uint8_t * data_buffer = new uint8_t[data_bytes];

	// uint8_t encoding[] = {0, 1, 3, 2};
	// Stringifyer strif(encoding);

	// Write skmer per skmer
	for (const vector<uint8_t *> & path : paths) {
		// cout << "path " << ++idx << "/" << paths.size() << endl;
		// Cleaning previous skmers/data
		memset(skmer_buffer, 0, max_skmer_bytes + 1);
		memset(data_buffer, 0, data_bytes);

		// Get the skmer minimizer position
		uint mini_pos = 0;
		uint8_t * mini_pos_pointer = path[0] + kmer_bytes + data_size;
		for (uint b=0 ; b<mini_pos_size ; b++) {
			mini_pos <<= 8;
			mini_pos += *mini_pos_pointer;
			mini_pos_pointer += 1;
		}

		// Usefull variables
		uint skmer_size = k - m - 1 + path.size();
		// uint skmer_bytes = (skmer_size + 3) / 4;
		uint skmer_offset = (4 - (skmer_size % 4)) % 4;

		// cout << "first kmer" << endl;
		// Save the first kmer
		memcpy(skmer_buffer, path[0], kmer_bytes);
		leftshift8(skmer_buffer, kmer_bytes, 2 * kmer_offset);
		rightshift8(skmer_buffer, kmer_bytes + 1, 2 * skmer_offset);
		// Save the first data
		memcpy(data_buffer, path[0] + kmer_bytes, data_size);

		// Compact kmer+data one by one
		for (uint kmer_idx = 1 ; kmer_idx<path.size() ; kmer_idx++) {
			// cout << "kmer " << kmer_idx << endl;
			uint8_t * kmer = path[kmer_idx];

			// Compute compaction position
			uint compact_nucl_pos = skmer_offset + k - m - 1 + kmer_idx;
			uint compact_byte = compact_nucl_pos / 4;
			uint compact_shift = 3 - (compact_nucl_pos % 4);
			// Compact the nucleotide
			uint8_t last_nucl = kmer[kmer_bytes - 1] & 0b11;
			skmer_buffer[compact_byte] |= last_nucl << (2 * compact_shift);
			// Copy data
			memcpy(data_buffer + kmer_idx * data_size, path[kmer_idx] + kmer_bytes, data_size);
		}

		uint8_t encoding[4] = {0, 1, 3, 2};
		Stringifyer strif(encoding);
		// cout << "write_compacted_sequence_without_mini" << endl;
		// Write everything in the file
		sm.write_compacted_sequence_without_mini(skmer_buffer, skmer_size, mini_pos, data_buffer);
	}

	// cout << "delete" << endl;

	delete[] skmer_buffer;
	delete[] data_buffer;
}

