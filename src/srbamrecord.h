#ifndef SRBAMRECORD_H
#define SRBAMRECORD_H

#include <vector>
#include <cstdint>
#include <iostream>
#include <algorithm>
#include <htslib/faidx.h>
#include "msa.h"
#include "bamutil.h"
#include "options.h"
#include "junction.h"
#include "svrecord.h"
#include "edgerecord.h"

/** class to store split read alignment record */
class SRBamRecord{
    public:
        int32_t mChr1;   ///< reference id part1 of read mapped
        int32_t mPos1;   ///< break point position of part1 read on reference
        int32_t mChr2;   ///< reference id part2 of read mapped
        int32_t mPos2;   ///< break point position of part2 read on reference
        int32_t mRstart; ///< starting mapping position on reference of one part read which is not -1
        int32_t mInslen; ///< insert size of two part of one read contributed
        int32_t mSVID;   ///< default -1, if allocated to an StructuralVariant, it is the index at which to store a StructuralVariant in vector
        size_t mID;      ///< hash value of the read name

    public:
        /** construct SRBamRecord object
         * @param chr1 reference id part1 of read mapped
         * @param pos1 break point position of part1 read on reference
         * @param chr2 reference id part2 of read mapped
         * @param pos2 break point position of part2 read on reference
         * @param rstart starting mapping position on reference of one part read which is not -1
         * @param inslen insert size of two part of one read contributed
         * @param id hash value of the read name
         */
        SRBamRecord(int32_t chr1, int32_t pos1, int32_t chr2, int32_t pos2, int32_t rstart, int32_t inslen, size_t id){
            mChr1 = chr1;
            mPos1 = pos1;
            mChr2 = chr2;
            mPos2 = pos2;
            mRstart = rstart;
            mInslen = inslen;
            mSVID = -1;
            mID = id;
        }

        /** SRBamRecord destructor */
        ~SRBamRecord(){}
        
        /** adjust orientation of SR read sequence to restore natural sequence in sample, only needed in 5to5 and 3to3 catenation
         * @param seq SR bam record seq
         * @param bpPoint if(SR mapped on little chr in translocation || mapped on higher coordinate in inversion) bpPoint = true;
         * @parm svt SV type
         */
        inline static void adjustOrientation(std::string& seq, bool bpPoint, int32_t svt){
            if((svt == 5 && bpPoint) || (svt == 6 && !bpPoint) ||
               (svt == 0 && bpPoint) || (svt == 1 && !bpPoint)){
                util::reverseComplement(seq);
            }
        }

        /** operator to output an SRBamRecord object to ostream
         * @param os reference of ostream object
         * @param sr reference of SRBamRecord object
         * @return reference of ostream
         */
        inline friend std::ostream& operator<<(std::ostream& os, const SRBamRecord& sr){
            os << "===============================================================\n";
            os << "Part1 of Split Read Reference ID: " << sr.mChr1 << "\n";
            os << "Part1 of Split Read Breakpoint Position on Reference: " << sr.mPos1 << "\n";
            os << "Part2 of Split Read Reference ID: " << sr.mChr2 << "\n";
            os << "Part2 of SPlit Read Breakpoint Position on Reference: " << sr.mPos2 << "\n";
            os << "Insert size contribured by Part1 and Part2 of Split Read: " << sr.mInslen << "\n";
            os << "ID of Structural Variant this Split Read contributed to: " << sr.mSVID << "\n";
            os << "Hash value of Split Read name: " << sr.mID << "\n";
            os << "===============================================================\n";
            return os;
        }

        /** operator to compare two SRBamRecord object
         * @param other reference of SRBamRecord
         * @return true if this SRBamRecord is less than other
         */
        inline bool operator<(const SRBamRecord& other) const {
            return mChr1 < other.mChr1 ||
               (mChr1 == other.mChr1 && mPos1 < other.mPos1) ||
               (mChr1 == other.mChr1 && mPos1 == other.mPos1 && mChr2 < other.mChr2) ||
               (mChr1 == other.mChr1 && mPos1 == other.mPos1 && mChr2 == other.mChr2 && mPos2 < other.mPos2);
        }
};

/** class to store SR information on each contig */
typedef std::vector<std::map<std::pair<int32_t, size_t>, int32_t>> ContigSRs; ///<[contig]<<mRstart, mID>, mSVID>

/** class to store SRBamRecord supporting various SVs */
class SRBamRecordSet{
    public:
        Options* mOpt;                              ///< pointer to Options object
        std::vector<std::vector<SRBamRecord>> mSRs; ///< vector to store SRBamRecords accordint the SV they support
        ContigSRs mSRMapPos;                        ///< SR mapping starting position on each contig with SV Type defined
        bool mSorted = false;                       ///< all SRBamRecords have been sorted if true
    public:
        /** SRBamRecordSet constructor 
         * @param opt pointer to Options
         * @param jctMap pointer to JunctionMap
         */
        SRBamRecordSet(Options* opt, JunctionMap* jctMap = NULL){
            mOpt = opt;
            mSRs.resize(9);
            mSRMapPos.resize(mOpt->contigNum);
            if(jctMap) classifyJunctions(jctMap);
        }

        /** SRBamRecordSet destructor */
        ~SRBamRecordSet(){}

        /** sort SRBamRecords in mSRs */
        void sortSRs(){
            for(auto& svt : mOpt->SVTSet){
                std::sort(mSRs[svt].begin(), mSRs[svt].end());
            }
            mSorted = true;
        }
        
        /** operator to output an SRBamRecordSet object to ostream
         * @param os reference of ostream object
         * @param sr reference of SRBamRecordSet object
         * @return reference of ostream
         */
        inline friend std::ostream& operator<<(std::ostream& os, const SRBamRecordSet& srs){
            for(uint32_t i = 0; i < srs.mSRs.size(); ++i){
                os << "SVT: " << i << "\n";
                for(uint32_t j = 0; j < srs.mSRs[i].size(); ++j){
                    os << "===== " << j + 1 << " =====\n";
                    os << srs.mSRs[i][j];
                }
                os << "\n";
            }
            return os;
        }

        /** class all Junction reads record in JunctionMap into SRBamRecord vector according to SV type they support
         * @param jctMap pointer to JunctionMap
         */
        void classifyJunctions(JunctionMap* jctMap);

        /** cluster SRBamRecord of one type SV and find all supporting SV of this type\n
         * step1: cluster SRBamRecord into different component, SRBamRecord with same chr1, pos1 abs diff in a limit[MaxReadSep](only considre nearing two) consists a component\n
         * step2: search each component for an clique, use the EdgeRecord in a component with the least weight as an seed, grow the clique as big as possible\n
         * @param srs reference of SRBamRecords which supporting SV type svt
         * @param svs SVSet used to store SV found
         * @param svt SV type to find in srs, range [0-8]
         */
        void cluster(std::vector<SRBamRecord>& srs, SVSet& svs, int32_t svt);
        
        /** cluster all SRBamRecord in SRBamRecordSet into their seperate supporting SVs */
        void cluster(SVSet& svs);

        /** a subroutine used to search all possible clique supporting an type of SV\n
         * step1: select seed, use the EdgeRecord in a component with the least weight as an seed of clique\n
         * step2: grow the clique, add another component if the expanded clique have starting/ending position diff smaller than a limit[MaxReadSep]\n
         * @param compEdge <compNumber, components> pairs clustered from SRBamRecords
         * @param srs reference of SRBamRecords which supporting SV type svt
         * @param svs SVSet used to store SV found
         * @param svt SV type to find in srs, range [0-8]
         */
        void searchCliques(std::map<int32_t, std::vector<EdgeRecord>>& compEdge, std::vector<SRBamRecord>& srs, SVSet& svs, int32_t svt);

        /** assembly reads of SR supporting each SV by MSA to get an consensus representation of SRs,\n
         * split align the consensus sequence against the constructed reference sequence to refine the breakpoint position
         * @param svs reference of SVSet
         */
        void assembleSplitReads(SVSet& svs);
};

#endif
