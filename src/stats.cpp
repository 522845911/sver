#include "stats.h"

Stats::Stats(Options* opt, int32_t n, int32_t refidx){
    mOpt = opt;
    mRefIdx = refidx;
    init(n);
}

Stats::Stats(int32_t n){
    mOpt = NULL;
    mRefIdx = -1;
    init(n);
}

void Stats::init(int n){
    mReadCnts.resize(n, ReadCount());
    mJctCnts.resize(n, JunctionCount());
    mSpnCnts.resize(n, SpanningCount());
    mCovCnts.resize(3 * n, {0, 0});
    mRefAlignedReadCount.resize(n, 0);
    mRefAlignedSpanCount.resize(n, 0);
}

uint32_t Stats::getAlignmentQual(Matrix2D<char>* alnResult, const uint8_t* qual){
    int32_t baseQualSum = 0;
    int32_t readPos = 0;
    int32_t alignedBases = 0;
    for(int j = 0; j < alnResult->ncol(); ++j){
        if(alnResult->get(1, j) != '-'){
            if(alnResult->get(0, j) != '-'){
                ++alignedBases;
                baseQualSum += qual[readPos];
            }
            ++readPos;
        }
    }
    return baseQualSum/alignedBases;
}

Stats* Stats::merge(const std::vector<Stats*>& sts, int32_t n){
    Stats* ret = new Stats(n);
    ret->mOpt = sts[0]->mOpt;
    for(int32_t j = 0; j < n; ++j){
        for(uint32_t i = 0; i < sts.size(); ++i){
            // RC
            ret->mReadCnts[j].mLeftRC += sts[i]->mReadCnts[j].mLeftRC;
            ret->mReadCnts[j].mRightRC += sts[i]->mReadCnts[j].mRightRC;
            ret->mReadCnts[j].mRC += sts[i]->mReadCnts[j].mRC;
            // JC
            ret->mJctCnts[j].mAlth1 += sts[i]->mJctCnts[j].mAlth1;
            ret->mJctCnts[j].mAlth2 += sts[i]->mJctCnts[j].mAlth2;
            ret->mJctCnts[j].mRefh1 += sts[i]->mJctCnts[j].mRefh1;
            ret->mJctCnts[j].mAltQual.insert(ret->mJctCnts[j].mAltQual.end(), sts[i]->mJctCnts[j].mAltQual.begin(), sts[i]->mJctCnts[j].mAltQual.end());
            ret->mJctCnts[j].mRefQual.insert(ret->mJctCnts[j].mRefQual.end(), sts[i]->mJctCnts[j].mRefQual.begin(), sts[i]->mJctCnts[j].mRefQual.end());
            // SC
            ret->mSpnCnts[j].mAlth1 += sts[i]->mSpnCnts[j].mAlth1;
            ret->mSpnCnts[j].mAlth2 += sts[i]->mSpnCnts[j].mAlth2;
            ret->mSpnCnts[j].mRefh1 += sts[i]->mSpnCnts[j].mRefh1;
            ret->mSpnCnts[j].mAltQual.insert(ret->mSpnCnts[j].mAltQual.end(), sts[i]->mSpnCnts[j].mAltQual.begin(), sts[i]->mSpnCnts[j].mAltQual.end());
            ret->mSpnCnts[j].mRefQual.insert(ret->mSpnCnts[j].mRefQual.end(), sts[i]->mSpnCnts[j].mRefQual.begin(), sts[i]->mSpnCnts[j].mRefQual.end());
            // Cov
            ret->mCovCnts[j].first += sts[i]->mCovCnts[j].first;
            ret->mCovCnts[j].second += sts[i]->mCovCnts[j].second;
            // REF Reads
            ret->mRefAlignedReadCount[j] += sts[i]->mRefAlignedReadCount[j];
            // REF Pairs
            ret->mRefAlignedSpanCount[j] += sts[i]->mRefAlignedSpanCount[j];
        }
    }
    return ret;
}

void Stats::stat(const SVSet& svs, const std::vector<std::vector<CovRecord>>& covRecs, const ContigBpRegions& bpRegs, const ContigSpanPoints& spPts, std::unordered_map<size_t, uint8_t>& transQuals, std::unordered_map<size_t, bool>& transClips){
    samFile* fp = sam_open(mOpt->bamfile.c_str(), "r");
    bam_hdr_t* h = sam_hdr_read(fp);
    util::loginfo("Start gathering coverage information on contig: " + std::string(h->target_name[mRefIdx]), mOpt->logMtx);
    // Flag breakpoint regions
    std::vector<bool> bpOccupied(h->target_len[mRefIdx], false);
    for(uint32_t i = 0; i < bpRegs[mRefIdx].size(); ++i){
        for(int32_t k = bpRegs[mRefIdx][i].mRegStart; k < bpRegs[mRefIdx][i].mRegEnd; ++k){
            bpOccupied[k] = true;
        }
    }
    // Flag spanning breakpoints
    std::vector<bool> spanBp(h->target_len[mRefIdx], false);
    for(uint32_t i = 0; i < spPts[mRefIdx].size(); ++i) spanBp[spPts[mRefIdx][i].mBpPos] = true;
    // Count reads
    hts_idx_t* idx = sam_index_load(fp, mOpt->bamfile.c_str());
    hts_itr_t* itr = sam_itr_queryi(idx, mRefIdx, 0, h->target_len[mRefIdx]);
    bam1_t* b = bam_init1();
    int32_t lastAlignedPos = 0;
    std::set<size_t> lastAlignedPosReads;
    AlignConfig alnCfg(5, -4, -4, -4, false, true);   
    const uint16_t COV_STAT_SKIP_MASK = (BAM_FSECONDARY | BAM_FQCFAIL | BAM_FDUP | BAM_FSUPPLEMENTARY | BAM_FUNMAP | BAM_FMUNMAP);
    std::unordered_map<size_t, uint8_t> qualities;
    std::unordered_map<size_t, bool> clip;
    while(sam_itr_next(fp, itr, b) >= 0){
        if(b->core.flag & COV_STAT_SKIP_MASK) continue;
        if(b->core.qual < mOpt->filterOpt->mMinGenoQual) continue;
        // Count aligned basepair (small InDels)
        bool hasSoftClip = false;
        bool hasClip = false;
        int32_t leadingSC = 0;
        int32_t rp = 0; // reference pos
        uint32_t* cigar = bam_get_cigar(b);
        for(uint32_t i = 0; i < b->core.n_cigar; ++i){
            int opint = bam_cigar_op(cigar[i]);
            int oplen = bam_cigar_oplen(cigar[i]);
            if(opint == BAM_CMATCH || opint == BAM_CDIFF || opint == BAM_CEQUAL){
                // Assign base counts to SVs
                int32_t rpos = rp + oplen;
                for(uint32_t rc = 0; rc < covRecs[mRefIdx].size(); ++rc){
                    if(covRecs[mRefIdx][rc].mStart < rpos &&  covRecs[mRefIdx][rc].mEnd > rp){
                        int32_t minPos = std::min(covRecs[mRefIdx][rc].mStart, rp);
                        int32_t maxPos = std::max(covRecs[mRefIdx][rc].mEnd, rpos);
                        for(int32_t ki = minPos; ki < maxPos; ++ki){
                            mCovCnts[covRecs[mRefIdx][rc].mID].first += 1;
                        }
                    }
                }
                rp += oplen;
            }else if(opint == BAM_CDEL){
                rp += oplen;
            }else if(opint == BAM_CREF_SKIP){
                rp += oplen;
            }else if(opint == BAM_CSOFT_CLIP){
                hasClip = true;
                hasSoftClip = true;
                if(i == 0) leadingSC = opint;
            }else if(opint == BAM_CHARD_CLIP) hasClip = true;
        }
        // Check read length for junction annotation
        if(b->core.l_qseq > 2 * mOpt->filterOpt->mMinFlankSize){
            bool bpvalid = false;
            int32_t rbegin = std::max(0, b->core.pos - leadingSC);
            for(int32_t k = rbegin; k < (b->core.pos + b->core.l_qseq) && k < (int32_t)h->target_len[mRefIdx]; ++k){
                if(bpOccupied[k]){
                    bpvalid = true;
                    break;
                }
            }
            if(bpvalid){
                // Fetch all relevant SVs
                auto itbp = std::lower_bound(bpRegs[mRefIdx].begin(), bpRegs[mRefIdx].end(), BpRegion(rbegin));
                for(; itbp != bpRegs[mRefIdx].end() && (b->core.pos + b->core.l_qseq) >= itbp->mBpPos; ++itbp){
                    // Read spans breakpoint, if this read mapping range contains itbp->mBpPos ± mMinFlankSize
                    if(hasSoftClip ||
                       ((!hasClip) && ((b->core.pos + mOpt->filterOpt->mMinFlankSize) <= itbp->mBpPos &&
                        (b->core.pos + b->core.l_qseq) >= (itbp->mBpPos + mOpt->filterOpt->mMinFlankSize)))){
                        std::string consProbe = itbp->mIsSVEnd ? svs[itbp->mID].mProbeEndC : svs[itbp->mID].mProbeBegC;
                        std::string refProbe = itbp->mIsSVEnd ? svs[itbp->mID].mProbeEndR : svs[itbp->mID].mProbeBegR;
                        // Get sequence
                        std::string readSeq = bamutil::getSeq(b);
                        SRBamRecord::adjustOrientation(readSeq, itbp->mIsSVEnd, itbp->mSVT);
                        // Compute alignment to alternative haplotype
                        Aligner* altAligner = new Aligner(consProbe, readSeq, &alnCfg);
                        Matrix2D<char>* altResult = new Matrix2D<char>();
                        int alnScore = altAligner->needle(altResult);
                        int matchThreshold = mOpt->filterOpt->mFlankQuality * consProbe.size() * alnCfg.mMatch + (1 - mOpt->filterOpt->mFlankQuality) * consProbe.size() * alnCfg.mMisMatch;
                        double scoreAlt = (double)alnScore / (double)matchThreshold;
                        // Compute alignment to reference haplotype
                        Aligner* refAligner = new Aligner(refProbe, readSeq, &alnCfg);
                        Matrix2D<char>* refResult = new Matrix2D<char>();
                        alnScore = refAligner->needle(refResult);
                        matchThreshold = mOpt->filterOpt->mFlankQuality * refProbe.size() * alnCfg.mMatch + (1 - mOpt->filterOpt->mFlankQuality) * refProbe.size() * alnCfg.mMisMatch;
                        double scoreRef = (double)alnScore / (double)matchThreshold;
                        // Any confident alignment?
                        if(scoreRef > 1 || scoreAlt > 1){
                            if(scoreRef > scoreAlt){// Account for reference bias
                                if(++mRefAlignedReadCount[itbp->mID] % 2){
                                    uint8_t* qual = bam_get_qual(b);
                                    uint32_t rq = getAlignmentQual(refResult, qual);
                                    if(rq >= mOpt->filterOpt->mMinGenoQual){
                                        uint8_t* hpptr = bam_aux_get(b, "HP");
                                        mJctCnts[itbp->mID].mRefQual.push_back(std::min(rq, (uint32_t)b->core.qual));
                                        if(hpptr){
                                            mOpt->libInfo->mIsHaploTagged = true;
                                            int hapv = bam_aux2i(hpptr);
                                            if(hapv == 1) ++mJctCnts[itbp->mID].mRefh1;
                                            else ++mJctCnts[itbp->mID].mRefh2;
                                        }
                                    }
                                }
                            }else{
                                uint8_t* qual = bam_get_qual(b);
                                uint32_t aq = getAlignmentQual(altResult, qual);
                                if(aq >= mOpt->filterOpt->mMinGenoQual){
                                    uint8_t* hpptr = bam_aux_get(b, "HP");
                                    mJctCnts[itbp->mID].mAltQual.push_back(std::min(aq, (uint32_t)b->core.qual));
                                    if(hpptr){
                                        mOpt->libInfo->mIsHaploTagged = true;
                                        int hapv = bam_aux2i(hpptr);
                                        if(hapv == 1) ++mJctCnts[itbp->mID].mAlth1;
                                        else ++mJctCnts[itbp->mID].mAlth2;
                                    }
                                }
                            }
                        }
                        delete altAligner;
                        altAligner = NULL;
                        delete altResult;
                        altResult = NULL;
                        delete refAligner;
                        refAligner = NULL;
                        delete refResult;
                        refResult = NULL;
                    }
                }
            }
        }
        // Read-count and spanning annotation
        if((!(b->core.flag & BAM_FPAIRED)) || covRecs[b->core.mtid].empty()) continue;
        // Clean-up the read store for identical alignment positions
        if(b->core.pos > lastAlignedPos){
            lastAlignedPosReads.clear();
            lastAlignedPos = b->core.pos;
        }
        if(firstInPair(b, lastAlignedPosReads)){
            // First read in pair
            lastAlignedPosReads.insert(svutil::hashString(bam_get_qname(b)));
            size_t hv = svutil::hashPairCurr(b);
            if(b->core.tid == b->core.mtid){
                qualities[hv] = b->core.qual;
                clip[hv] = hasSoftClip;
            }else{
                mOpt->traMtx.lock();
                transQuals[hv] = b->core.qual;
                transClips[hv] = hasSoftClip;
                mOpt->traMtx.unlock();
            }
        }else{
            // Second read in pair
            size_t hv = svutil::hashPairMate(b);
            uint8_t pairQual = 0;
            bool pairClip = false;
            if(b->core.tid == b->core.mtid){
                if(qualities.find(hv) == qualities.end()) continue;
                pairQual = std::min(qualities[hv], b->core.qual);
                if(clip[hv] || hasSoftClip) pairClip = true;
                qualities[hv] = 0;
                clip[hv] = false;
            }else{
                mOpt->traMtx.lock();
                if(transQuals.find(hv) == transQuals.end()){
                    mOpt->traMtx.unlock();
                    continue;
                }
                pairQual = std::min(transQuals[hv], b->core.qual);
                if(transClips[hv] || hasSoftClip) pairClip = true;
                transQuals[hv] = 0;
                transClips[hv] = false;
                mOpt->traMtx.unlock();
            }
            // Pair quality
            if(pairQual < mOpt->filterOpt->mMinGenoQual) continue; // Low quality pair
            // Read-depth fragment counting
            if(b->core.tid == b->core.mtid){
                // Count mid point (fragment counting)
                int32_t midPos = b->core.pos + bam_cigar2rlen(b->core.n_cigar, bam_get_cigar(b))/2;
                // Assign fragment counts to SVs
                for(uint32_t rc = 0; rc < covRecs[mRefIdx].size(); ++rc){
                    if(midPos >= covRecs[mRefIdx][rc].mStart && midPos < covRecs[mRefIdx][rc].mEnd){
                        mCovCnts[covRecs[mRefIdx][rc].mID].second += 1;
                        break;
                    }
                }
            }
            // Spanning counting
            int32_t outerISize = b->core.pos + b->core.l_qseq - b->core.mpos;
            // Normal spanning pair
            if((!pairClip) && (DPBamRecord::getSVType(b) == 2) && outerISize >= mOpt->libInfo->mMinNormalISize &&
               outerISize <= mOpt->libInfo->mMaxNormalISize && b->core.mtid == b->core.tid){
                // Take 80% of the outersize as the spanned interval
                int32_t spanlen = 0.8 * outerISize;
                int32_t pbegin = b->core.mpos;
                int32_t st = pbegin + 0.1 * outerISize;
                bool spanvalid = false;
                for(int32_t i = st; i < st + spanlen && i < (int32_t)h->target_len[mRefIdx]; ++i){
                    if(spanBp[i]){
                        spanvalid = true;
                        break;
                    }
                }
                if(spanvalid){
                    // Fetch all relevant SVs
                    auto itspan = std::lower_bound(spPts[mRefIdx].begin(), spPts[mRefIdx].end(), SpanPoint(st));
                    for(; itspan != spPts[mRefIdx].end() && (st + spanlen) >= itspan->mBpPos; ++itspan){
                        // Account for reference bias
                        if(++mRefAlignedSpanCount[itspan->mID] % 2){
                            uint8_t* hpptr = bam_aux_get(b, "HP");
                            mSpnCnts[itspan->mID].mRefQual.push_back(pairQual);
                            if(hpptr){
                                mOpt->libInfo->mIsHaploTagged = true;
                                int hap = bam_aux2i(hpptr);
                                if(hap == 1) ++mSpnCnts[itspan->mID].mRefh1;
                                else ++mSpnCnts[itspan->mID].mRefh2;
                            }
                        }
                    }
                }
            }
            // Abnormal spanning coverage
            if((DPBamRecord::getSVType(b) != 2) || outerISize < mOpt->libInfo->mMinNormalISize || outerISize > mOpt->libInfo->mMaxNormalISize || b->core.tid != b->core.mtid){
                // Get SV type
                int32_t svt =  DPBamRecord::getSVType(b, mOpt);
                if(svt == -1) continue;
                // Spanning a breakpoint?
                bool spanvalid = false;
                int32_t pbegin = b->core.pos;
                int32_t pend = std::min(b->core.pos + mOpt->libInfo->mMaxNormalISize, (int32_t)h->target_len[mRefIdx]);
                if(b->core.flag & BAM_FREVERSE){
                    pbegin = std::max(0, b->core.pos + b->core.l_qseq - mOpt->libInfo->mMaxNormalISize);
                    pend = std::min(b->core.pos + b->core.l_qseq, (int32_t)h->target_len[mRefIdx]);
                }
                for(int32_t i = pbegin; i < pend; ++i){
                    if(spanBp[i]){
                        spanvalid = true;
                        break;
                    }
                }
                if(spanvalid){
                    // Fetch all relevant SVs
                    auto itspan = std::lower_bound(spPts[mRefIdx].begin(), spPts[mRefIdx].end(), SpanPoint(pbegin));
                    for(; itspan != spPts[mRefIdx].end() && pend >= itspan->mBpPos; ++itspan){
                        if(svt == itspan->mSVT){
                            uint8_t* hpptr = bam_aux_get(b, "HP");
                            mSpnCnts[itspan->mID].mAltQual.push_back(pairQual);
                            if(hpptr){
                                mOpt->libInfo->mIsHaploTagged = true;
                                int hap = bam_aux2i(hpptr);
                                if(hap == 1) ++mSpnCnts[itspan->mID].mAlth1;
                                else ++mSpnCnts[itspan->mID].mAlth2;
                            }
                        }
                    }
                }
            }
        }
    }
    // Compute read counts
    int32_t lastID = svs.size();
    for(uint32_t id = 0; id < svs.size(); ++id){
        if(svs[id].mSize <= mOpt->filterOpt->mMinInDelSize){
            mReadCnts[id].mRC = mCovCnts[id].first;
            mReadCnts[id].mLeftRC = mCovCnts[id + lastID].first;
            mReadCnts[id].mRightRC = mCovCnts[id + 2 * lastID].first;
        }else{
            mReadCnts[id].mRC = mCovCnts[id].second;
            mReadCnts[id].mLeftRC = mCovCnts[id + lastID].second;
            mReadCnts[id].mRightRC = mCovCnts[id + 2 * lastID].second;
        }
    }
    util::loginfo("Finish gathering coverage information on contig: " + std::string(h->target_name[mRefIdx]), mOpt->logMtx);
    // Clean-up
    sam_close(fp);
    bam_hdr_destroy(h);
    bam_destroy1(b);
    hts_itr_destroy(itr);
    hts_idx_destroy(idx);
}
