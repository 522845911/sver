#ifndef ANNOTATOR_H
#define ANNOTATOR_H

#include "util.h"
#include "stats.h"
#include <htslib/tbx.h>
#include <htslib/kstring.h>

/** SV breakpoint coverage annotator */
class Annotator{
    public:
        Options* mOpt; ///< pointer to Options

    public:
        /** constructor of Annotator
         * @param opt pointer to Options object
         */
        Annotator(Options* opt) : mOpt(opt) {}
        
        /** destructor of Annotator */
        ~Annotator(){}

        /** annotate SV coverage
         * @param svs reference of SVRecords
         */
        Stats* covAnnotate(SVSet& svs);

        /** annotate SV gene information
         * @param svs reference of SVRecords
         * @param gl reference of GeneInfoList
         */
        void geneAnnotate(SVSet& svs, GeneInfoList& gl);

        /** get first overlap of an region against an region set
         * @param s region sets
         * @param p region to query
         * @return iterator to an region overlap with p or end
         */
        static std::set<std::pair<int32_t, int32_t>>::iterator getFirstOverlap(const std::set<std::pair<int32_t, int32_t>>& s, const std::pair<int32_t, int32_t>& p);
};

#endif
