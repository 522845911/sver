#include "annotator.h"
#include "ThreadPool.h"

std::set<std::pair<int32_t, int32_t>>::iterator Annotator::getFirstOverlap(const std::set<std::pair<int32_t, int32_t>>& s, const std::pair<int32_t, int32_t>&p){
    auto itsv= std::upper_bound(s.begin(), s.end(), p);
    auto upIter = itsv;
    if(itsv != s.begin()){
        auto lowIter = --itsv;
        bool lowGet = false;
        while(p.first <= lowIter->second){
            if(itsv == s.begin()){
                return itsv;
            }
            --lowIter;
            lowGet = true;
        }
        if(lowGet){
            return ++lowIter;
        }
    }
    bool upGet = false;
    while(upIter != s.end() && p.second >= upIter->first){
        ++upIter;
        upGet = true;
    }
    if(upGet){
        return --upIter;
    }
    return s.end();
}

Stats* Annotator::covAnnotate(std::vector<SVRecord>& svs){
    // Open file handler
    samFile* fp = sam_open(mOpt->bamfile.c_str(), "r");
    hts_set_fai_filename(fp, mOpt->genome.c_str());
    bam_hdr_t* h = sam_hdr_read(fp);
    faidx_t* fai = fai_load(mOpt->genome.c_str());
    hts_idx_t* idx = sam_index_load(fp, mOpt->bamfile.c_str());
    // Find Ns in reference genome
    util::loginfo("Starting gathering N regions in reference");
    std::vector<std::set<std::pair<int32_t, int32_t>>> nreg(h->n_targets);
    for(auto& refIndex : mOpt->svRefID){
        int32_t refSeqLen = -1;
        char* seq = faidx_fetch_seq(fai, h->target_name[refIndex], 0, h->target_len[refIndex], &refSeqLen);
        bool nrun = false;
        int nstart = refSeqLen;
        for(int32_t i = 0; i < refSeqLen; ++i){
            if(seq[i] != 'N' || seq[i] != 'n'){
                if(nrun){
                    nreg[refIndex].insert(std::make_pair(nstart, i - 1));
                    nrun = false;
                }
            }else{
                if(!nrun){
                    nstart = i;
                    nrun = true;
                }
            }
        }
        // Insert last possible Ns region
        if(nrun) nreg[refIndex].insert(std::make_pair(nstart, refSeqLen - 1));
        if(seq) free(seq);
    }
    util::loginfo("Finish gathering N regions in reference");
    util::loginfo("Start extracting left/middle/right regions for each SV");
    // Add control regions
    std::vector<std::vector<CovRecord>> covRecs(h->n_targets); //coverage records of 3-part of each SV events
    int32_t lastID = svs.size();
    for(auto itsv = svs.begin(); itsv != svs.end(); ++itsv){
        int halfsize = (itsv->mSVEnd - itsv->mSVStart) / 2; 
        if(itsv->mSVT >= 4) halfsize = 500;
        // Left control region
        CovRecord covLeft;
        covLeft.mID = lastID + itsv->mID;
        covLeft.mStart = std::max(0, itsv->mSVStart - halfsize);
        covLeft.mEnd = itsv->mSVStart;
        auto itp = getFirstOverlap(nreg[itsv->mChr1], {covLeft.mStart, covLeft.mEnd});
        while(itp != nreg[itsv->mChr1].end()){
            covLeft.mStart = std::max(itp->first - halfsize, 0);
            covLeft.mEnd = itp->first;
            itp = getFirstOverlap(nreg[itsv->mChr1], {covLeft.mStart, covLeft.mEnd});
        }
        covRecs[itsv->mChr1].push_back(covLeft);
        // Actual SV region
        CovRecord covMiddle;
        covMiddle.mID = itsv->mID;
        covMiddle.mStart = itsv->mSVStart;
        covMiddle.mEnd = itsv->mSVEnd;
        if(itsv->mSVT >= 4){
            covMiddle.mStart = std::max(itsv->mSVStart - halfsize, 0);
            covMiddle.mEnd = std::min((int32_t)h->target_len[itsv->mChr2], itsv->mSVStart + halfsize);
        }
        itsv->mSize = itsv->mSVEnd - itsv->mSVStart;
        if(itsv->mSVT >= 5) itsv->mSize = 666666666;
        covRecs[itsv->mChr1].push_back(covMiddle);
        // Right control region
        CovRecord covRight;
        covRight.mID = 2 * lastID + itsv->mID;
        covRight.mStart = itsv->mSVEnd;
        covRight.mEnd = std::min((int32_t)h->target_len[itsv->mChr2], itsv->mSVEnd + halfsize);
        itp = getFirstOverlap(nreg[itsv->mChr2], {covRight.mStart, covRight.mEnd});
        while(itp != nreg[itsv->mChr2].end()){
            covRight.mStart = itp->second;
            covRight.mEnd = itp->second + halfsize;
            itp = getFirstOverlap(nreg[itsv->mChr2], {covRight.mStart, covRight.mEnd});
        }
        covRecs[itsv->mChr2].push_back(covRight);
    }
    // Sort Coverage Records
    for(auto& refIndex : mOpt->svRefID){
        std::sort(covRecs[refIndex].begin(), covRecs[refIndex].end());
    }
    util::loginfo("Finish extracting left/middle/right regions for each SV");
    // Preprocess REF and ALT
    ContigBpRegions bpRegion(h->n_targets);
    std::vector<int32_t> refAlignedReadCount(svs.size());
    std::vector<int32_t> refAlignedSpanCount(svs.size());
    util::loginfo("Start extracting breakpoint regions of each precisely classified SV");
    for(auto itsv = svs.begin(); itsv != svs.end(); ++ itsv){
        if(!itsv->mPrecise) continue;
        // Iterate all break point
        for(int bpPoint = 0; bpPoint < 2; ++bpPoint){
            int32_t regChr, regStart, regEnd, bpPos;
            if(bpPoint){// SV ending position region
                regChr = itsv->mChr2;
                regStart = std::max(0, itsv->mSVEnd - mOpt->filterOpt->mMinFlankSize);
                regEnd = std::min(itsv->mSVEnd + mOpt->filterOpt->mMinFlankSize, (int32_t)h->target_len[itsv->mChr2]);
                bpPos = itsv->mSVEnd;
            }else{// SV starting position region
                regChr = itsv->mChr1;
                regStart = std::max(0, itsv->mSVStart - mOpt->filterOpt->mMinFlankSize);
                regEnd = std::min(itsv->mSVStart + mOpt->filterOpt->mMinFlankSize, (int32_t)h->target_len[itsv->mChr2]);
                bpPos = itsv->mSVStart;
            }
            BpRegion br;
            br.mIsSVEnd = bpPoint;
            br.mBpPos = bpPos;
            br.mID = itsv->mID;
            br.mRegEnd = regEnd;
            br.mRegStart = regStart;
            br.mSVT = itsv->mSVT;
            bpRegion[regChr].push_back(br);
        }
    }
    // Sort all BpRegion by breakpoint position on reference
    for(auto& refIndex : mOpt->svRefID) std::sort(bpRegion[refIndex].begin(), bpRegion[refIndex].end());
    util::loginfo("Finish extracting breakpoint regions of each precisely classified SV");
    // Get spanning point regions
    util::loginfo("Start extracting PE supported breakpoints of each SV");
    ContigSpanPoints spanPoint;
    spanPoint.resize(mOpt->contigNum);
    for(auto itsv = svs.begin(); itsv != svs.end(); ++itsv){
        if(itsv->mPESupport == 0) continue;
        spanPoint[itsv->mChr1].push_back(SpanPoint(itsv->mSVStart, itsv->mSVT, itsv->mID));
        spanPoint[itsv->mChr2].push_back(SpanPoint(itsv->mSVEnd, itsv->mSVT, itsv->mID));
    }
    for(uint32_t i = 0; i < spanPoint.size(); ++i) std::sort(spanPoint[i].begin(), spanPoint[i].end());
    util::loginfo("Finish extracting PE supported breakpoints of each SV");
    // Clean-up
    sam_close(fp);
    hts_idx_destroy(idx);
    fai_destroy(fai);
    // Translocation quality and clip status recorders
    std::unordered_map<size_t, uint8_t> transQuals;
    std::unordered_map<size_t, bool> transClips;
    // Get coverage from each contig in parallel
    ThreadPool::ThreadPool pool(std::min(mOpt->nthread, (int32_t)mOpt->svRefID.size()));
    std::vector<Stats*> covStats(mOpt->svRefID.size());
    std::vector<std::future<void>> statRets(mOpt->svRefID.size());
    int32_t i = 0;
    for(auto& refidx: mOpt->svRefID){
        covStats[i] = new Stats(mOpt, svs.size(), refidx);
        statRets[i] = pool.enqueue(&Stats::stat, covStats[i], std::ref(svs), std::ref(covRecs), std::ref(bpRegion), std::ref(spanPoint), std::ref(transQuals), std::ref(transClips));
        ++i;
    }
    for(auto& e: statRets) e.get();
    Stats* finalStat = Stats::merge(covStats, svs.size());
    for(auto& e: covStats) delete e;
    return finalStat;
}

void Annotator::geneAnnotate(SVSet& svs, GeneInfoList& gl){
    gl.resize(svs.size());
    std::vector<std::string> vstr;
    kstring_t rec = {0, 0, 0};
    htsFile* fp = hts_open(mOpt->annodb.c_str(), "r");
    tbx_t* tbx = tbx_index_load(mOpt->annodb.c_str());
    for(uint32_t i = 0; i < svs.size(); ++i){
        gl[i].mChr1 = svs[i].mNameChr1;
        gl[i].mChr2 = svs[i].mNameChr2;
        gl[i].mPos1 = svs[i].mSVStart;
        gl[i].mPos2 = svs[i].mSVEnd;
        gl[i].mSVT = svs[i].mSVT;
        gl[i].mDPS = svs[i].mPESupport;
        gl[i].mSRS = svs[i].mSRSupport;
        hts_itr_t* itr = tbx_itr_queryi(tbx, tbx_name2id(tbx, svs[i].mNameChr1.c_str()), svs[i].mSVStart, svs[i].mSVStart + 1);
        while(tbx_itr_next(fp, tbx, itr, &rec) >= 0){
            util::split(rec.s, vstr, "\t");
            gl[i].mTrans1.push_back(vstr[6] + "(" + vstr[4] + ":" +vstr[5] + ")"); 
            gl[i].mGene1 = vstr[7];
            gl[i].mStrand1.push_back(vstr[3]);
        }
        tbx_itr_destroy(itr);
        itr = tbx_itr_queryi(tbx, tbx_name2id(tbx, svs[i].mNameChr2.c_str()), svs[i].mSVEnd, svs[i].mSVEnd + 1);
        while(tbx_itr_next(fp, tbx, itr, &rec) >= 0){
            util::split(rec.s, vstr, "\t");
            gl[i].mTrans2.push_back(vstr[6] + "(" + vstr[4] + ":" +vstr[5] + ")"); 
            gl[i].mGene2 = vstr[7];
            gl[i].mStrand2.push_back(vstr[3]);
        }
        tbx_itr_destroy(itr);
    }
    tbx_destroy(tbx);
    hts_close(fp);
}
