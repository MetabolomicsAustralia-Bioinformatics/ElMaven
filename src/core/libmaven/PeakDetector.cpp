#include <unordered_map>
#include <unordered_set>

#include <omp.h>

#include <boost/bind.hpp>

#include "adductdetection.h"
#include "datastructures/adduct.h"
#include "classifierNeuralNet.h"
#include "datastructures/mzSlice.h"
#include "peakdetector.h"
#include "EIC.h"
#include "mzUtils.h"
#include "Compound.h"
#include "mzSample.h"
#include "constants.h"
#include "classifier.h"
#include "massslicer.h"
#include "peakFiltering.h"
#include "groupFiltering.h"
#include "mavenparameters.h"
#include "mzMassCalculator.h"
#include "isotopeDetection.h"
#include "Scan.h"

PeakDetector::PeakDetector() {
    _mavenParameters = NULL;
}

PeakDetector::PeakDetector(MavenParameters* mp) {
    _mavenParameters = mp;
}

void PeakDetector::sendBoostSignal(const string &progressText,
                                   unsigned int completed_slices,
                                   int total_slices)
{
    boostSignal(progressText, completed_slices, total_slices);
}

void PeakDetector::resetProgressBar() {
    _zeroStatus = true;
}

vector<EIC*> PeakDetector::pullEICs(const mzSlice* slice,
                                    const std::vector<mzSample*>& samples,
                                    const MavenParameters* mp,
                                    bool filterUnselectedSamples)
{
    vector<mzSample*> vsamples;
    for (auto sample : samples) {
        if (sample == nullptr)
            continue;
        if (filterUnselectedSamples && !sample->isSelected)
            continue;
        vsamples.push_back(sample);
    }

    vector<EIC*> eics;
#pragma omp parallel
    {
        vector<EIC*> sharedEics;
#pragma omp for nowait
        for (unsigned int i = 0; i < vsamples.size(); i++) {
            // Samples been selected
            mzSample* sample = vsamples[i];
            // getting the slice with which EIC has to be pulled
            Compound* c = slice->compound;

            EIC* e = nullptr;

            if (!slice->srmId.empty()) {
                e = sample->getEIC(slice->srmId, mp->eicType);
            } else if (c && c->precursorMz() > 0 && c->productMz() > 0) {
                e = sample->getEIC(c->precursorMz(),
                                   c->collisionEnergy(),
                                   c->productMz(),
                                   mp->eicType,
                                   mp->filterline,
                                   mp->amuQ1,
                                   mp->amuQ3);
            } else {
                e = sample->getEIC(slice->mzmin,
                                   slice->mzmax,
                                   sample->minRt,
                                   sample->maxRt,
                                   1,
                                   mp->eicType,
                                   mp->filterline);
            }

            if (e) {
                // if eic exists, perform smoothing
                EIC::SmootherType smootherType =
                    (EIC::SmootherType)mp->eic_smoothingAlgorithm;
                e->setSmootherType(smootherType);

                // set appropriate baseline parameters
                if (mp->aslsBaselineMode) {
                    e->setBaselineMode(EIC::BaselineMode::AsLSSmoothing);
                    e->setAsLSSmoothness(mp->aslsSmoothness);
                    e->setAsLSAsymmetry(mp->aslsAsymmetry);
                } else {
                    e->setBaselineMode(EIC::BaselineMode::Threshold);
                    e->setBaselineSmoothingWindow(mp->baseline_smoothingWindow);
                    e->setBaselineDropTopX(mp->baseline_dropTopX);
                }
                e->computeBaseline();
                e->reduceToRtRange(slice->rtmin, slice->rtmax);
                e->setFilterSignalBaselineDiff(mp->minSignalBaselineDifference);
                e->getPeakPositions(mp->eic_smoothingWindow);

                // push eic to shared EIC vector
                sharedEics.push_back(e);
            }
        }
#pragma omp critical
        eics.insert(begin(eics), begin(sharedEics), end(sharedEics));
    }
    return eics;
}

void PeakDetector::processFeatures(const vector<Compound*>& identificationSet)
{
    _mavenParameters->showProgressFlag = true;

    // find average scan time
    _mavenParameters->setAverageScanTime();

    MassSlicer massSlicer(_mavenParameters);
    massSlicer.findFeatureSlices();

    // sort the slices based on their intensities to enurmerate good slices
    sort(massSlicer.slices.begin(),
         massSlicer.slices.end(),
         mzSlice::compIntensity);

    if (massSlicer.slices.empty())
        return;

    sendBoostSignal("Peak Detection", 0, 1);

    processSlices(massSlicer.slices, "groups");
    delete_all(massSlicer.slices);

    // identify features with known targets
    identifyFeatures(identificationSet);
}

void PeakDetector::identifyFeatures(const vector<Compound*>& identificationSet)
{
    if (identificationSet.empty())
        return;

    sendBoostSignal("Preparing libraries for identification…", 0, 0);
    MassSlicer massSlicer(_mavenParameters);
    if (_mavenParameters->pullIsotopesFlag && _mavenParameters->searchAdducts) {
        massSlicer.generateIsotopeSlices(identificationSet);
        massSlicer.generateAdductSlices(identificationSet, true, false);
    } else if (_mavenParameters->pullIsotopesFlag) {
        massSlicer.generateIsotopeSlices(identificationSet);
    } else if (_mavenParameters->searchAdducts) {
        massSlicer.generateAdductSlices(identificationSet);
    } else {
        massSlicer.generateCompoundSlices(identificationSet);
    }

    GroupFiltering groupFiltering(_mavenParameters);
    vector<PeakGroup> toBeMerged;
    auto iter = _mavenParameters->allgroups.begin();
    while(iter != _mavenParameters->allgroups.end()) {
        auto& group = *iter;
        bool matchFound = false;
        for (auto slice : massSlicer.slices) {
            if (mzUtils::withinXMassCutoff(slice->mz,
                                           group.meanMz,
                                           _mavenParameters->massCutoffMerge)) {
                PeakGroup groupWithTarget(group);
                groupWithTarget.setCompound(slice->compound);
                groupWithTarget.setAdduct(slice->adduct);
                groupWithTarget.setIsotope(slice->isotope);

                // we should filter the annotated group based on its RT, if the
                // user has restricted RT range
                auto rtDiff = groupWithTarget.expectedRtDiff();
                if (_mavenParameters->identificationMatchRt
                    && rtDiff > _mavenParameters->identificationRtWindow) {
                    continue;
                }

                // since we are creating groups with targets, we should ensure
                // that the parent ion forms of these groups should at least
                // pass MS2 filtering criteria, if enabled
                if (_mavenParameters->matchFragmentationFlag
                    && groupWithTarget.adduct()->isParent()
                    && groupWithTarget.isotope().isParent()
                    && groupWithTarget.ms2EventCount > 0
                    && groupFiltering.filterByMS2(groupWithTarget)) {
                    continue;
                }

                matchFound = true;
                toBeMerged.push_back(groupWithTarget);
            }
        }

        if (matchFound) {
            iter = _mavenParameters->allgroups.erase(iter);
        } else {
            ++iter;
        }

       sendBoostSignal("Identifying features using the given compound set…",
                       iter - _mavenParameters->allgroups.begin(),
                       _mavenParameters->allgroups.size());
    }
    delete_all(massSlicer.slices);

    if (!toBeMerged.empty()) {
        _mavenParameters->allgroups.insert(
            _mavenParameters->allgroups.begin(),
            make_move_iterator(toBeMerged.begin()),
            make_move_iterator(toBeMerged.end()));
    }

    performMetaGrouping();

    GroupFiltering filter(_mavenParameters);
    for (auto& group : _mavenParameters->allgroups) {
        if (group.isGhost())
            continue;

        if (group.isIsotope() || group.isAdduct())
            continue;

        if (group.hasCompoundLink()
            && _mavenParameters->pullIsotopesFlag
            && _mavenParameters->filterIsotopesAgainstParent) {
            sendBoostSignal("Filtering isotopologues…", 0, 0);
            filter.filterBasedOnParent(
                group,
                GroupFiltering::ChildFilterType::Isotope,
                _mavenParameters->maxIsotopeScanDiff,
                _mavenParameters->minIsotopicCorrelation,
                _mavenParameters->massCutoffMerge);
        }
        if (group.hasCompoundLink()
            && _mavenParameters->searchAdducts
            && _mavenParameters->filterAdductsAgainstParent) {
            sendBoostSignal("Filtering adducts…", 0, 0);
            filter.filterBasedOnParent(
                group,
                GroupFiltering::ChildFilterType::Adduct,
                _mavenParameters->adductSearchWindow,
                _mavenParameters->adductPercentCorrelation,
                _mavenParameters->massCutoffMerge);
        }
    }
}

void PeakDetector::processCompounds(vector<Compound*> compounds)
{
    if (compounds.size() == 0)
        return;

    sendBoostSignal("Preparing libraries for search…", 0, 0);
    string setName = "groups";

    bool srmTransitionPresent = false;
    auto srmTransitionPos = find_if(begin(compounds),
                                    end(compounds),
                                    [](Compound* compound) {
                                        return compound->type()
                                               == Compound::Type::MRM;
                                    });
    if (srmTransitionPos != end(compounds)) {
        setName = "transitions";
        srmTransitionPresent = true;
    }

    MassSlicer massSlicer(_mavenParameters);
    if (_mavenParameters->pullIsotopesFlag
        && _mavenParameters->searchAdducts
        && !srmTransitionPresent) {
        setName = "isotopologues and adducts";
        massSlicer.generateIsotopeSlices(compounds);
        massSlicer.generateAdductSlices(compounds, true, false);
    } else if (_mavenParameters->pullIsotopesFlag && !srmTransitionPresent) {
        setName = "isotopologues";
        massSlicer.generateIsotopeSlices(compounds);
    } else if (_mavenParameters->searchAdducts && !srmTransitionPresent) {
        setName = "adducts";
        massSlicer.generateAdductSlices(compounds);
    } else {
        massSlicer.generateCompoundSlices(compounds);
    }

    processSlices(massSlicer.slices, setName);
    delete_all(massSlicer.slices);

    performMetaGrouping();

    GroupFiltering filter(_mavenParameters);
    for (auto& group : _mavenParameters->allgroups) {
        if (group.isGhost())
            continue;

        if (group.isIsotope() || group.isAdduct())
            continue;

        if (group.hasCompoundLink()
            && _mavenParameters->pullIsotopesFlag
            && _mavenParameters->filterIsotopesAgainstParent
            && !srmTransitionPresent) {
            sendBoostSignal("Filtering isotopologues…", 0, 0);
            filter.filterBasedOnParent(
                group,
                GroupFiltering::ChildFilterType::Isotope,
                _mavenParameters->maxIsotopeScanDiff,
                _mavenParameters->minIsotopicCorrelation,
                _mavenParameters->compoundMassCutoffWindow);
        }
        if (group.hasCompoundLink()
            && _mavenParameters->searchAdducts
            && _mavenParameters->filterAdductsAgainstParent
            && !srmTransitionPresent) {
            sendBoostSignal("Filtering adducts…", 0, 0);
            filter.filterBasedOnParent(
                group,
                GroupFiltering::ChildFilterType::Adduct,
                _mavenParameters->adductSearchWindow,
                _mavenParameters->adductPercentCorrelation,
                _mavenParameters->compoundMassCutoffWindow);
        }
    }
}

void PeakDetector::processSlices(vector<mzSlice*>& slices, string setName)
{
    if (slices.empty())
        return;

    // shared `MavenParameters` object
    auto mp = make_shared<MavenParameters>(*_mavenParameters);

    // lambda that adds detected groups to mavenparameters
    auto detectGroupsForSlice = [&](vector<EIC*>& eics, mzSlice* slice) {
        vector<PeakGroup> peakgroups =
            EIC::groupPeaks(eics,
                            slice,
                            mp,
                            PeakGroup::IntegrationType::Automated);

        // we do not filter non-parent adducts or non-parent isotopologues
        if (slice->adduct == nullptr
            || slice->isotope.isNone()
            || (slice->adduct->isParent() && slice->isotope.isParent())) {
            GroupFiltering groupFiltering(_mavenParameters, slice);
            groupFiltering.filter(peakgroups);
        }
        if (peakgroups.empty())
            return;

        _mavenParameters->allgroups.insert(
            _mavenParameters->allgroups.begin(),
            make_move_iterator(peakgroups.begin()),
            make_move_iterator(peakgroups.end()));
    };

    _mavenParameters->allgroups.clear();
    sort(slices.begin(), slices.end(), mzSlice::compIntensity);
    for (unsigned int s = 0; s < slices.size(); s++) {
        if (_mavenParameters->stop)
            break;

        mzSlice* slice = slices[s];
        vector<EIC*> eics = pullEICs(slice,
                                     _mavenParameters->samples,
                                     _mavenParameters);

        if (_mavenParameters->clsf->hasModel())
            _mavenParameters->clsf->scoreEICs(eics);

        float eicMaxIntensity = 0;
        for (auto eic : eics) {
            float max = 0;
            switch (static_cast<PeakGroup::QType>(_mavenParameters->peakQuantitation))
            {
            case PeakGroup::AreaTop:
                max = eic->maxAreaTopIntensity;
                break;
            case PeakGroup::Area:
                max = eic->maxAreaIntensity;
                break;
            case PeakGroup::Height:
                max = eic->maxIntensity;
                break;
            case PeakGroup::AreaNotCorrected:
                max = eic->maxAreaNotCorrectedIntensity;
                break;
            case PeakGroup::AreaTopNotCorrected:
                max = eic->maxAreaTopNotCorrectedIntensity;
                break;
            default:
                max = eic->maxIntensity;
                break;
            }

            if (max > eicMaxIntensity)
                eicMaxIntensity = max;
        }

        // we only filter parent peak-groups on group filtering parameters
        if ((slice->adduct == nullptr
             || slice->isotope.isNone()
             || (slice->adduct->isParent() && slice->isotope.isParent()))
            && eicMaxIntensity < _mavenParameters->minGroupIntensity) {
            delete_all(eics);
            continue;
        }

        // TODO: maybe adducts should have their own filters?
        bool isIsotope = !(slice->isotope.isParent()
                           && slice->adduct->isParent());
        PeakFiltering peakFiltering(_mavenParameters, isIsotope);
        peakFiltering.filter(eics);

        detectGroupsForSlice(eics, slice);

        // cleanup
        delete_all(eics);

        if (_mavenParameters->allgroups.size()
            > _mavenParameters->limitGroupCount) {
            cerr << "Group limit exceeded!" << endl;
            break;
        }

        if (_zeroStatus) {
            sendBoostSignal("Status", 0, 1);
            _zeroStatus = false;
        }

        if (_mavenParameters->showProgressFlag) {
            string progressText = "Found "
                                  + to_string(_mavenParameters->allgroups.size())
                                  + " "
                                  + setName;
            sendBoostSignal(progressText,
                            s + 1,
                            std::min((int)slices.size(),
                                     _mavenParameters->limitGroupCount));
        }
    }
}

// filter for top N ranked parent peak-groups per compound
void _keepNBestRanked(unordered_map<Compound*, vector<size_t>>& compoundGroups,
                      vector<PeakGroup>& container,
                      int nBest)
{
    for (auto& elem : compoundGroups) {
        vector<size_t>& groupIndexes = elem.second;
        if (groupIndexes.size() <= nBest)
            continue;

        sort(begin(groupIndexes), end(groupIndexes), [&](size_t a, size_t b) {
            PeakGroup& group = container[a];
            PeakGroup& otherGroup = container[b];
            return group.groupRank > otherGroup.groupRank;
        });
        for (size_t i = nBest; i < groupIndexes.size(); ++i)
            container.erase(begin(container) + groupIndexes[i]);
        groupIndexes.erase(begin(groupIndexes) + nBest, end(groupIndexes));
    }
}

pair<map<size_t, size_t>, vector<size_t>>
_matchParentsToChildren(vector<size_t>& parentIndexes,
                        vector<size_t>& childIndexes,
                        vector<PeakGroup>& container,
                        function<string(PeakGroup*)> nameFunc)
{
    map<string, vector<size_t>> nameGroupedChildren;
    for (auto index : childIndexes) {
        PeakGroup& child = container[index];
        string subType = nameFunc(&child);
        if (nameGroupedChildren.count(subType) == 0)
            nameGroupedChildren[subType] = {};
        nameGroupedChildren[subType].push_back(index);
    }

    // lambda: checks whether the RT difference between the i-th and the k-th
    // peak-groups is less than that of the j-th and the k-th peak-groups
    auto lessRtDel = [&container](size_t i, size_t j, size_t k) {
        return abs(container[i].meanRt - container[k].meanRt)
               < abs(container[j].meanRt - container[k].meanRt);
    };

    // lambda: sorts object vector based on RT difference b/w object and subject
    auto sortObjects = [&lessRtDel](vector<size_t>& objects, size_t subject) {
        sort(begin(objects),
             end(objects),
             [&lessRtDel, subject](size_t o1, size_t o2) {
                 return lessRtDel(o1, o2, subject);
             });
    };

    // lambda: for a given parent or child group (subject), assign the most
    // preferred child or parent group (object), respectively; in case of a
    // clash, the loser must select its next preference (recursively); one
    // important assumption is that the number of competing subjects is less
    // than (or equal to) the number of available objects
    function<void(size_t,
                  map<size_t, size_t>&,
                  map<size_t, size_t>&,
                  map<size_t, vector<size_t>>&)> findPreferredMatch;
    findPreferredMatch =
        [&lessRtDel, &findPreferredMatch]
        (size_t subject,
         map<size_t, size_t>& subjectsWithObjects,
         map<size_t, size_t>& objectsWithSubjects,
         map<size_t, vector<size_t>>& priorityLists) -> void {
            auto priorityList = priorityLists[subject];
            for (size_t object : priorityList) {
                if (objectsWithSubjects.count(object)) {
                    auto competingSubject = objectsWithSubjects[object];
                    if (lessRtDel(subject, competingSubject, object)) {
                        subjectsWithObjects[subject] = object;
                        objectsWithSubjects[object] = subject;

                        auto iter = subjectsWithObjects.find(competingSubject);
                        if(iter != end(subjectsWithObjects))
                            subjectsWithObjects.erase(iter);
                        findPreferredMatch(competingSubject,
                                           subjectsWithObjects,
                                           objectsWithSubjects,
                                           priorityLists);
                        break;
                    }
                } else {
                    subjectsWithObjects[subject] = object;
                    objectsWithSubjects[object] = subject;
                    break;
                }
            }
        };

    vector<size_t> orphans;
    map<size_t, size_t> nonOrphans;
    for (auto& elem : nameGroupedChildren) {
        auto& childIndexes = elem.second;

        map<size_t, size_t> childrenWithParents;
        map<size_t, size_t> parentsWithChildren;
        map<size_t, vector<size_t>> priorityLists;
        if (childIndexes.size() <= parentIndexes.size()) {
            for (size_t childIndex : childIndexes) {
                vector<size_t> copyOfParentIndexes = parentIndexes;
                sortObjects(copyOfParentIndexes, childIndex);
                priorityLists[childIndex] = copyOfParentIndexes;
            }

            for (size_t childIndex : childIndexes) {
                findPreferredMatch(childIndex,
                                   childrenWithParents,
                                   parentsWithChildren,
                                   priorityLists);
            }
        } else {
            for (size_t parentIndex : parentIndexes) {
                vector<size_t> copyOfChildIndexes = childIndexes;
                sortObjects(copyOfChildIndexes, parentIndex);
                priorityLists[parentIndex] = copyOfChildIndexes;
            }

            for (size_t parentIndex : parentIndexes) {
                findPreferredMatch(parentIndex,
                                   parentsWithChildren,
                                   childrenWithParents,
                                   priorityLists);
            }

            // keep track of children that could not find parents
            for (auto childIndex : childIndexes) {
                if (childrenWithParents.count(childIndex) == 0)
                    orphans.push_back(childIndex);
            }
        }
        nonOrphans.insert(begin(childrenWithParents), end(childrenWithParents));
    }
    return make_pair(nonOrphans, orphans);
}

void PeakDetector::performMetaGrouping()
{
    sendBoostSignal("Performing meta-grouping…", 0, 0);

    // separate parent groups, then filter for the N-best groups per compound
    unordered_map<Compound*, vector<size_t>> parentCompounds;
    for (size_t i = 0; i < _mavenParameters->allgroups.size(); ++i) {
        PeakGroup& group = _mavenParameters->allgroups[i];
        Compound* compound = group.getCompound();
        if (compound == nullptr)
            continue;

        if (group.isotope().isParent() && group.adduct()->isParent()) {
            if (parentCompounds.count(compound) == 0)
                parentCompounds[compound] = {};
            parentCompounds[compound].push_back(i);
        }
    }
    _keepNBestRanked(parentCompounds,
                     _mavenParameters->allgroups,
                     _mavenParameters->eicMaxGroups);

    // put isotopologues and adducts into separate buckets
    unordered_map<Compound*, vector<size_t>> nonParentIsotopologues;
    unordered_map<Compound*, vector<size_t>> nonParentAdducts;
    for (size_t i = 0; i < _mavenParameters->allgroups.size(); ++i) {
        PeakGroup& group = _mavenParameters->allgroups[i];
        Compound* compound = group.getCompound();
        if (compound == nullptr)
            continue;

        if (group.isIsotope()) {
            if (nonParentIsotopologues.count(compound) == 0)
                nonParentIsotopologues[compound] = {};
            nonParentIsotopologues[compound].push_back(i);
        } else if (group.isAdduct()) {
            if (nonParentAdducts.count(compound) == 0)
                nonParentAdducts[compound] = {};
            nonParentAdducts[compound].push_back(i);
        }
    }

    // enumerate group IDs for all remaining peak-groups
    int groupId = 1;
    for (auto& group : _mavenParameters->allgroups)
        group.setGroupId(groupId++);

    if (nonParentIsotopologues.empty() && nonParentAdducts.empty())
        return;

    // lambda: given a compound and its child indexes, clubs them with their
    // most likely parent-group if possible, otherwise adds them to a ghost
    auto makeMeta =
        [this](Compound* compound,
               vector<size_t> childIndexes,
               unordered_map<Compound*, vector<size_t>> parentCompounds,
               function<string(PeakGroup*)> nameFunc) {
            auto& container = _mavenParameters->allgroups;
            vector<size_t> orphans;
            map<size_t, size_t> nonOrphans;
            if (parentCompounds.count(compound) > 0) {
                auto& parentIndexes = parentCompounds[compound];
                auto result = _matchParentsToChildren(parentIndexes,
                                                      childIndexes,
                                                      container,
                                                      nameFunc);
                nonOrphans = result.first;
                orphans = result.second;
            } else {
                for (auto index : childIndexes)
                    orphans.push_back(index);
            }

            unordered_map<size_t, vector<size_t>> metaGroups;
            for (auto& elem : nonOrphans) {
                size_t parentIndex = elem.second;
                size_t childIndex = elem.first;
                if (metaGroups.count(parentIndex) == 0)
                    metaGroups[parentIndex] = {};
                metaGroups[parentIndex].push_back(childIndex);
            }

            if (!orphans.empty()) {
                // for orphans, create a ghost, that will act as an empty parent
                PeakGroup parentGroup(
                    make_shared<MavenParameters>(*_mavenParameters),
                    PeakGroup::IntegrationType::Ghost);
                container.push_back(parentGroup);

                // set an appropriate slice for ghost parent
                mzSlice slice;
                slice.compound = compound;
                slice.calculateMzMinMax(
                    _mavenParameters->compoundMassCutoffWindow,
                    _mavenParameters->getCharge(compound));
                slice.calculateRTMinMax(false, 0.0f);
                container.back().setSlice(slice);

                size_t totalSize = container.size();
                container.back().setGroupId(totalSize);
                metaGroups[totalSize - 1] = {};
                for (auto child : orphans)
                    metaGroups[totalSize - 1].push_back(child);
            }

            return metaGroups;
        };

    unordered_map<Compound*, unordered_map<size_t, vector<size_t>>> metaGroups;

    // find isotope meta-groups
    for (auto& elem : nonParentIsotopologues) {
        Compound* compound = elem.first;
        auto& isotopeIndexes = elem.second;
        auto metaIsotopeGroups = makeMeta(compound,
                                          isotopeIndexes,
                                          parentCompounds,
                                          [](PeakGroup* group) {
                                              return group->isotope().name;
                                          });
        metaGroups[compound] = metaIsotopeGroups;
    }

    // find adduct meta-groups
    for (auto& elem : nonParentAdducts) {
        Compound* compound = elem.first;
        auto& adductIndexes = elem.second;
        auto metaAdductGroups = makeMeta(compound,
                                         adductIndexes,
                                         parentCompounds,
                                         [](PeakGroup* group) {
                                             return group->adduct()->getName();
                                         });
        if (metaGroups.count(compound) > 0) {
            auto& existingMetaGroups = metaGroups[compound];
            for (auto& elem : metaAdductGroups) {
                size_t parentIndex = elem.first;
                vector<size_t>& childIndexes = elem.second;
                if (existingMetaGroups.count(parentIndex) > 0) {
                    vector<size_t>& existingChildIndexes =
                        existingMetaGroups[parentIndex];
                    existingChildIndexes.insert(end(existingChildIndexes),
                                                begin(childIndexes),
                                                end(childIndexes));
                } else {
                    existingMetaGroups[parentIndex] = childIndexes;
                }
            }
        } else {
            metaGroups[compound] = metaAdductGroups;
        }
    }

    // perform final meta-grouping and queue children to be erased
    vector<size_t> indexesToErase;
    for (auto& elem : metaGroups) {
        auto& compoundMetaGroups = elem.second;
        for (auto& metaGroup : compoundMetaGroups) {
            PeakGroup& parent = _mavenParameters->allgroups[metaGroup.first];
            auto& childIndexes = metaGroup.second;
            for (auto childIndex : childIndexes) {
                PeakGroup& child = _mavenParameters->allgroups[childIndex];
                if (child.isIsotope())
                    parent.addIsotopeChild(child);
                if (child.isAdduct())
                    parent.addAdductChild(child);
                indexesToErase.push_back(childIndex);
            }
        }
    }

    // the following index-overwrite removal does not preserve order
    sort(begin(indexesToErase), end(indexesToErase), greater<size_t>());
    for (size_t index : indexesToErase) {
        _mavenParameters->allgroups[index] = _mavenParameters->allgroups.back();
        _mavenParameters->allgroups.pop_back();
    }
}
