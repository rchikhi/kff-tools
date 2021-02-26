#include <vector>
#include <string>
#include <cstring>
#include <cmath>
#include <algorithm>

#include "sequences.hpp"
#include "compact.hpp"


using namespace std;


Compact::Compact() {
	input_filename = "";
	output_filename = "";

	load_mem_size = 1;
	loading_memory = new uint8_t[load_mem_size];
	kmer_buffer = new uint8_t[1];
	skmer_buffer = new uint8_t[1];
	data_buffer = new uint8_t[1];
}


Compact::~Compact() {
	delete[] loading_memory;
	delete[] kmer_buffer;
	delete[] skmer_buffer;
	delete[] data_buffer;
}


void Compact::cli_prepare(CLI::App * app) {
	this->subapp = app->add_subcommand("compact", "Read a kff file and try to compact the kmers from minimizer sections. The available ram must be sufficent to load a complete minimizer section into memory");
	CLI::Option * input_option = subapp->add_option("-i, --infile", input_filename, "Input kff file to compact.");
	input_option->required();

	CLI::Option * out_option = subapp->add_option("-o, --outfile", output_filename, "Kff to write (must be different from the input)");
	out_option->required();
}

void Compact::compact(string input, string output) {
	// Read the encoding of the first file and push it as outcoding
	Kff_file infile(input, "r");

	// Write header of the output
	Kff_file outfile(output, "w");
	outfile.write_encoding(infile.encoding);
	// Set metadata
	uint8_t * metadata = new uint8_t[infile.metadata_size];
	infile.read_metadata(metadata);
	outfile.write_metadata(infile.metadata_size, metadata);
	delete[] metadata;

	long buffer_size = 1048576; // 1 MB
	char buffer[1048576];

	// Read section by section
	char section_type = infile.read_section_type();
	while(not infile.fs.eof()) {
		switch (section_type) {
			// Write the variables that change from previous sections (possibly sections from other input files)
			case 'v':
			{
				// Read the values
				Section_GV in_sgv(&infile);
				Section_GV out_sgv(&outfile);

				// Verify the presence and value of each variable in output
				for (auto& tuple : in_sgv.vars) {
					out_sgv.write_var(tuple.first, tuple.second);
				}

				in_sgv.close();
				out_sgv.close();

				delete[] kmer_buffer;
				delete[] skmer_buffer;
				delete[] data_buffer;
				uint k = infile.global_vars["k"];
				uint max = infile.global_vars["max"];
				uint data_size = infile.global_vars["data_size"];

				kmer_buffer = new uint8_t[(k - 1 + max) / 4 + 2];
				skmer_buffer = new uint8_t[(k - 1 + max) / 4 + 2];
				data_buffer = new uint8_t[max * data_size];
			}
			break;

			// copy the sequence section from input to output
			case 'r':
			{
				uint in_max = infile.global_vars["max"];
				uint out_max = outfile.global_vars["max"];
				if (in_max != out_max) {
					Section_GV sgv(&outfile);
					sgv.write_var("max", in_max);
					sgv.close();
				}

				// Analyse the section size to prepare copy
				cerr << "WARNING: Raw sections are not compacted !" << endl;
				auto begin_byte = infile.fs.tellp();
				if (not infile.jump_next_section()) {
					cerr << "Error inside of the input file." << endl;
					cerr << "Impossible to jump over the raw section at byte " << begin_byte << endl;
					exit(1);
				}
				auto end_byte = infile.fs.tellp();
				long size = end_byte - begin_byte;
				infile.fs.seekp(begin_byte);

				// copy from input to output
				while (size > 0) {
					size_t to_copy = size > buffer_size ? buffer_size : size;

					infile.fs.read(buffer, to_copy);
					outfile.fs.write(buffer, to_copy);

					size -= to_copy;
				}
			}
			case 'm':
			{
				// Verify the ability to compact
				uint m = outfile.global_vars["m"];
				uint k = outfile.global_vars["k"];
				uint max = outfile.global_vars["max"];

				if (max < 2 * (k - m)) {
					max = 2 * (k - m);
					Section_GV sgv(&outfile);
					sgv.write_var("max", max);
					sgv.close();

					delete[] kmer_buffer;
					delete[] skmer_buffer;
					delete[] data_buffer;
					uint data_size = outfile.global_vars["data_size"];

					kmer_buffer = new uint8_t[(k - 1 + max) / 4 + 2];
					skmer_buffer = new uint8_t[(k - 1 + max) / 4 + 2];
					cout << "alloc buffer " << ((k - 1 + max) / 4 + 2) << endl;
					data_buffer = new uint8_t[max * data_size];
				}

				// Open the input minimizer section
				Section_Minimizer sm(&infile);
				uint8_t * mini = new uint8_t[(m+3)/4];
				memcpy(mini, sm.minimizer, (m+3)/4);
				// Load all the blocks
				this->kmer_nbs.clear();
				this->mini_pos.clear();
				this->loadSectionBlocks(sm, infile);
				// Compute paths from left to right for each available sequence
				vector<vector<uint> > paths = this->link_kmers(sm.nb_blocks, infile);
				sm.close();
				// Assemble and write the paths
				this->compact_and_save(paths, outfile, mini, infile.global_vars["max"]);

				delete[] mini;
			}
			break;

			default:
				cerr << "Unknown section type " << section_type << " in file " << input_filename << endl;
				exit(2);
		}

		// Prepare next section
		section_type = infile.read_section_type();
	}

	infile.close();
	outfile.close();
}

void Compact::loadSectionBlocks(Section_Minimizer & sm, Kff_file & infile) {
	cout << "--- load ---" << endl;
	uint k = infile.global_vars["k"];
	uint m = infile.global_vars["m"];
	uint max_kmers = infile.global_vars["max"];
	uint data_size = infile.global_vars["data_size"];
	
	uint skmer_nucl_bytes = (max_kmers + k - 1 - m + 3) / 4;
	uint skmer_data_bytes = max_kmers * data_size;
	uint max_block_size = skmer_nucl_bytes + skmer_data_bytes;

	// Realloc
	uint max_size = max_block_size * sm.nb_blocks;
	if (load_mem_size <  max_size) {
		delete[] loading_memory;
		loading_memory = new uint8_t[max_size];
		load_mem_size = max_size;
	}

	// Read all the sequences
  for (uint64_t i=0 ; i<sm.nb_blocks ; i++) {
  	// Save sequences and data
    uint64_t minimizer_position;
    uint64_t nb_kmers = sm.read_compacted_sequence_without_mini(
    		this->loading_memory + i * max_block_size,
    		this->loading_memory + i * max_block_size + skmer_nucl_bytes,
    		minimizer_position);

    // Save values
    this->kmer_nbs.push_back(nb_kmers);
    this->mini_pos.push_back(minimizer_position);
  }
}

vector<vector<uint> > Compact::link_kmers(uint nb_kmers, Kff_file & infile) {
	cout << "--- Create links between sequences ---" << endl;
	// Usefull variables
	uint k = infile.global_vars["k"];
	uint m = infile.global_vars["m"];
	uint max_kmers = infile.global_vars["max"];
	uint data_size = infile.global_vars["data_size"];
	
	uint skmer_nucl_bytes = (max_kmers + k - 1 - m + 3) / 4;
	uint skmer_data_bytes = max_kmers * data_size;
	uint max_block_size = skmer_nucl_bytes + skmer_data_bytes;

	uint8_t * sub_seq = new uint8_t[k/4 + 1];

	// map <prefix_size, map<64bits_prefix_value, vector<sequence_idx> > >
	unordered_map<uint, unordered_map<uint64_t, vector<uint> > > prefix_bins;
	vector<uint> present_bins;

	// Distribute kmers into positional bins.
	// Bin x means x nucleotides inside of the prefix
	uint kmer_idx = 0;
	for (uint & mini_idx : this->mini_pos) {
		// Create absent bin
		if (prefix_bins.find(mini_idx) == prefix_bins.end()) {
			prefix_bins[mini_idx] = unordered_map<uint64_t, vector<uint> >();
			present_bins.push_back(mini_idx);
		}

		// Compute the prefix
		uint8_t * seq = loading_memory + kmer_idx * max_block_size;
		subsequence(seq,
				k - 1 + this->kmer_nbs[kmer_idx] - m,
				sub_seq, 0, (k-1)-m-1);
		// Prefix to int value
		uint64_t prefix_val = seq_to_uint(sub_seq, k - m - 1);

		// Insert the prefix
		prefix_bins[mini_idx][prefix_val].push_back(kmer_idx);
		kmer_idx += 1;
	}

	sort(present_bins.begin(), present_bins.end());
	reverse(present_bins.begin(), present_bins.end());

	// Create paths of overlaping sequences
	// Each sequence is present in only one path
	vector<vector<uint> > paths;
	vector<uint> current_path;

	while(present_bins.size() > 0) {
		// Get a start point
		unordered_map<uint64_t, vector<uint> > & first_idx_bin = prefix_bins[present_bins[0]];
		uint64_t first_prefix = first_idx_bin.begin()->first;
		vector<uint> & first_kmers_idx = first_idx_bin.begin()->second;

		// Start a path
		current_path.push_back(first_kmers_idx[0]);
		first_kmers_idx.erase(first_kmers_idx.begin());
		if (first_kmers_idx.size() == 0) {
			first_idx_bin.erase(first_prefix);
			if (first_idx_bin.size() == 0) {
				prefix_bins.erase(present_bins[0]);
				present_bins.erase(present_bins.begin());
			}
		}

		// Loop over sequences to concatenate
		while (true) {
			uint last_idx = current_path.back();
			uint last_kmer_nb = this->kmer_nbs[last_idx];
			uint last_mini_idx = this->mini_pos[last_idx];
			uint8_t * last_seq = loading_memory + last_idx * max_block_size;
			uint last_seq_size = k - 1 + last_kmer_nb - m;

			// Compute the next bin to look for
			int suff_idx = (int)last_mini_idx - (int)last_kmer_nb;
			if (suff_idx < 0 or prefix_bins.find((uint)suff_idx) == prefix_bins.end()) {
				// cerr << "No sequence with the good minimizer idx" << endl;
				break;
			}

			// Extract the (k-1)-suffix of the last element of the path
			subsequence(last_seq, last_seq_size, sub_seq, last_kmer_nb, last_seq_size-1);
			uint64_t suff_val = seq_to_uint(sub_seq, last_seq_size - last_kmer_nb);

			// Look for the elements with the good (k-1)-prefix
			if (prefix_bins[suff_idx].find(suff_val) == prefix_bins[suff_idx].end()) {
				// cerr << "No sequence with the good prefix" << endl;
				break;
			}
			// Iterate over candidates
			bool valid_candidate = false;
			uint compaction_idx;
			vector<uint> & candidates = prefix_bins[suff_idx][suff_val];
			if (last_seq_size - 1 - suff_idx <= 32) {
				valid_candidate = true;
				compaction_idx = 0;
				current_path.push_back(candidates[0]);
			} else {
				cerr << "TODO: implement verification for suffix larger than 32 nucleotides" << endl;
				exit(1);
			}

			// Remove concatenated from candidates
			if (valid_candidate) {
				candidates.erase(candidates.begin() + compaction_idx);
				// Remove the (k-1) mer from possibilities if no remaining prefix
				if (candidates.size() == 0) {
					prefix_bins[suff_idx].erase(suff_val);

					// Remove the minimizer indice for kmer if there is no available kmer in this bin.
					if (prefix_bins[suff_idx].size() == 0) {
						prefix_bins.erase(suff_idx);
						present_bins.erase(std::remove(present_bins.begin(), present_bins.end(), suff_idx), present_bins.end());
					}
				}
			}
		}

		// Add the path
		paths.push_back(current_path);
		current_path = vector<uint>();
	}

	delete[] sub_seq;

	cout << "paths " << paths.size() << " " << paths[0].size() << endl << endl;
	return paths;
}

void Compact::compact_and_save(vector<vector<uint> > paths, Kff_file & outfile, uint8_t * minimizer, uint input_max_kmers) {
	// Global variables
	uint k = outfile.global_vars["k"];
	uint m = outfile.global_vars["m"];
	uint max_kmers = outfile.global_vars["max"];
	uint data_size = outfile.global_vars["data_size"];
	// Loacal variables
	uint skmer_nucl_bytes = (input_max_kmers + k - 1 - m + 3) / 4;
	uint skmer_data_bytes = input_max_kmers * data_size;
	uint max_block_size = skmer_nucl_bytes + skmer_data_bytes;

	Section_Minimizer sm(&outfile);
	sm.write_minimizer(minimizer);
	// Construct the path from right to left
	for (vector<uint> & path : paths) {
		uint compacted_size = 0;
		cout << "NEW PATH" << endl;

		// Start the skmer with the last sequence
		uint last_idx = path[path.size()-1];
		uint last__used_nucl = k - 1 - m + this->kmer_nbs[last_idx];
		uint last__used_bytes = (last__used_nucl + 3) / 4;
		uint last__first_used_byte = skmer_nucl_bytes - last__used_bytes;
		cout << "skmer_nucl_bytes " << skmer_nucl_bytes << endl;
		cout << last__first_used_byte << " " << last__used_bytes << endl;
		memcpy(
			skmer_buffer + last__first_used_byte,
			loading_memory + last_idx * max_block_size,
			last__used_bytes
		);
		// exit(0);
		compacted_size += last__used_nucl;
		cout << "last sequence " << last_idx << " used nucl " << last__used_nucl << " bytes " << last__used_bytes << endl;
		cout << (uint)skmer_buffer[skmer_nucl_bytes-2] << " " << (uint)skmer_buffer[skmer_nucl_bytes-1] << endl;

		// Concatenate all the other nucleotides
		for (int idx=path.size()-2 ; idx>=0 ; idx--) {
			cout << "Compact size " << compacted_size << endl;
			uint seq_idx = path[idx];
			uint seq_size = k - 1 - m + this->kmer_nbs[seq_idx];
			uint seq_bytes = (seq_size + 3) / 4;
			uint8_t * seq = loading_memory + seq_idx * max_block_size;

			cout << "idx " << seq_idx << " nb nucl " << seq_size << " nb bytes " << seq_bytes << endl;
			cout << (uint)seq[0] << " " << (uint)seq[1] << endl;

			// copy in kmer buffer
			memcpy(kmer_buffer+1, seq, seq_bytes);

			// Shift the kmer buffer
			uint first_used_nucl_idx = 4 + (4 - (seq_size % 4) % 4);
			uint last_used_nucl_idx = first_used_nucl_idx + this->kmer_nbs[seq_idx] - 1;
			cout << "nucl bounds " << first_used_nucl_idx << " " << last_used_nucl_idx << endl;
			// uint last_shifted_byte = 
			int shift = (compacted_size % 4) - ((k - 1 - m) % 4);
			cout << shift << endl;
			if (shift <= 0) {
				rightshift8(kmer_buffer+1, seq_bytes, -2 * shift);
			} else {
				leftshift8(kmer_buffer, seq_bytes+1, 2 * shift);
			}
			first_used_nucl_idx -= shift;
			last_used_nucl_idx -= shift;
			cout << "shifted bounds " << first_used_nucl_idx << " " << last_used_nucl_idx << endl;
			uint first_used_byte = first_used_nucl_idx / 4;
			uint last_used_byte = last_used_nucl_idx / 4;
			uint used_length = last_used_byte - first_used_byte + 1;
			cout << "used bytes " << first_used_byte << " " << last_used_byte << " length " << used_length << endl;
			// Copy the bytes needed
			uint compacted_first_byte = skmer_nucl_bytes - 1 - (compacted_size + this->kmer_nbs[seq_idx]) / 4;
			cout << "compacted_first_byte " << compacted_first_byte << endl;
			cout << (uint)skmer_buffer[compacted_first_byte] << " " << (uint)skmer_buffer[compacted_first_byte+1] << endl;
			memcpy(skmer_buffer+compacted_first_byte, kmer_buffer + first_used_byte, used_length-1);
			// merge the middle byte
			skmer_buffer[compacted_first_byte+used_length-1] = fusion8(
					kmer_buffer[last_used_byte],
					skmer_buffer[compacted_first_byte+used_length-1],
					2* ((last_used_nucl_idx+1) % 4));
			cout << "last used_byte " << last_used_byte << " " << (uint)kmer_buffer[last_used_byte] << endl;
			cout << "fusion idx " << ((last_used_nucl_idx+1) % 4) << endl;
		cout << "fusioned bytes " << (uint)kmer_buffer[last_used_byte] << " " << (uint)skmer_buffer[compacted_first_byte+used_length-1] << endl;

			// update values
			compacted_size += this->kmer_nbs[seq_idx];
			cout << compacted_size << endl;
			// exit(0);
		}

		uint compacted_first_byte = skmer_nucl_bytes - 1 - compacted_size / 4;
		cout << "first byte " << compacted_first_byte << endl;
		// cout << skmer_buffer
		sm.write_compacted_sequence_without_mini(
			skmer_buffer + compacted_first_byte,
			compacted_size, this->mini_pos[path[0]], nullptr);
	}

	sm.close();
}

// GCT|TGA


void Compact::exec() {
	this->compact(input_filename, output_filename);
}
