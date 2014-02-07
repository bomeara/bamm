#include "Model.h"
#include "MbRandom.h"
#include "Tree.h"
#include "Settings.h"
#include "Prior.h"
#include "Node.h"
#include "BranchEvent.h"
#include "BranchHistory.h"
#include "Log.h"

#include <string>
#include <fstream>
#include <cstdlib>


double Model::mhColdness = 1.0;


Model::Model(MbRandom* rng, Tree* tree, Settings* settings, Prior* prior) :
    _rng(rng), _tree(tree), _settings(settings), _prior(prior), _gen(0)
{
    // Reduce weird autocorrelation of values at start by calling RNG
    // a few times. TODO: Why is there a weird autocorrelation?
    for (int i = 0; i < 100; i++)
        _rng->uniformRv();

    // Event location scale is relative to the maximum root-to-tip length
    _scale = _settings->getUpdateEventLocationScale() *
        _tree->maxRootToTipLength();

    _updateEventRateScale = _settings->getUpdateEventRateScale();
    _localGlobalMoveRatio = _settings->getLocalGlobalMoveRatio();
    
    _poissonRatePrior = _settings->getPoissonRatePrior();

    // Initialize event rate to generate expected number of prior events
    _eventRate = 1 / _settings->getPoissonRatePrior();

    _acceptCount = 0;
    _rejectCount = 0;
    _acceptLast = -1;

    _lastDeletedEventMapTime = 0;
}


Model::~Model()
{
}


void Model::initializeModelFromEventDataFile()
{
    std::string inputFileName(_settings->getEventDataInfile());
    std::ifstream inputFile(inputFileName.c_str());

    if (!inputFile.good()) {
        log(Error) << "<<" << inputFileName << ">> is a bad file name.\n";
        std::exit(1);
    }

    log() << "Initializing model from <<" << inputFileName << ">>\n";

    std::string species1;
    std::string species2;
    double eTime;

    int eventCount = 0;
    while (inputFile) {
        inputFile >> species1;
        inputFile >> species2;
        inputFile >> eTime;

        // Read the model-specific parameters
        readModelSpecificParameters(inputFile);

        // TODO: Might need to getline here to read last \n

        Node* x = NULL;
        
        if ((species1 != "NA") && (species2 != "NA")) {
            x = _tree->getNodeMRCA(species1.c_str(), species2.c_str());
        } else if ((species1 != "NA") && (species2 == "NA")) {
            x = _tree->getNodeByName(species1.c_str());
        } else {
            log(Error) << "Either both species are NA or the second species "
                << "is NA\nwhile reading the event data file.";
            std::exit(1);
        }

        if (x == _tree->getRoot()) {
            // Set the root event with model-specific parameters
            setRootEventWithReadParameters();
        } else {
            double deltaT = x->getTime() - eTime;
            double newMapTime = x->getMapStart() + deltaT;

            BranchEvent* newEvent =
                newBranchEventWithReadParameters(x, newMapTime);
            newEvent->getEventNode()->getBranchHistory()->
                addEventToBranchHistory(newEvent);

            _eventCollection.insert(newEvent);
            forwardSetBranchHistories(newEvent);
            setMeanBranchParameters();
        }

        eventCount++;
    }

    inputFile.close();

    log() << "Read a total of " << eventCount << " events.\n";
    log() << "Added " << _eventCollection.size() << " "
          << "pre-defined events to tree, plus root event.\n";
}


void Model::forwardSetBranchHistories(BranchEvent* x)
{
    // If there is another event occurring more recent (closer to tips),
    // do nothing. Even just sits in BranchHistory but doesn't affect
    // state of any other nodes.

    // This seems circular, but what else to do?
    // given an event (which references the node defining the branch on which
    // event occurs) you get the corresponding branch history and the last
    // event since the events will have been inserted in the correct order.

    Node* myNode = x->getEventNode();

    if (x == _rootEvent) {
        forwardSetHistoriesRecursive(myNode->getLfDesc());
        forwardSetHistoriesRecursive(myNode->getRtDesc());
    } else if (x == myNode->getBranchHistory()->getLastEvent()) {
        // If true, x is the most tip-wise event on branch.
        myNode->getBranchHistory()->setNodeEvent(x);

        // If myNode is not a tip
        if (myNode->getLfDesc() != NULL && myNode->getRtDesc() != NULL) {
            forwardSetHistoriesRecursive(myNode->getLfDesc());
            forwardSetHistoriesRecursive(myNode->getRtDesc());
        }
        // Else: node is a tip; do nothing
    }
    // Else: there is another more tipwise event on the same branch; do nothing
}


/*
    If this works correctly, this will take care of the following:
    1. if a new event is created or added to tree,
       this will forward set all branch histories from the insertion point
    2. If an event is deleted, you find the next event rootwards,
       and call forwardSetBranchHistories from that point. It will replace
       settings due to the deleted node with the next rootwards node.
*/

void Model::forwardSetHistoriesRecursive(Node* p)
{
    // Get event that characterizes parent node
    BranchEvent* lastEvent = p->getAnc()->getBranchHistory()->getNodeEvent();

    // Set the ancestor equal to the event state of parent node:
    p->getBranchHistory()->setAncestralNodeEvent(lastEvent);

    // Ff no events on the branch, go down to descendants and do same thing;
    // otherwise, process terminates (because it hits another event on branch
    if (p->getBranchHistory()->getNumberOfBranchEvents() == 0) {
        p->getBranchHistory()->setNodeEvent(lastEvent);

        if (p->getLfDesc() != NULL) {
            forwardSetHistoriesRecursive(p->getLfDesc());
        }

        if (p->getRtDesc() != NULL) {
            forwardSetHistoriesRecursive(p->getRtDesc());
        }
    }
}


void Model::addEventToTree()
{
    double aa = _tree->getRoot()->getMapStart();
    double bb = _tree->getTotalMapLength();
    double x = _rng->uniformRv(aa, bb);
                
    addEventToTree(x);
}


// Adds event to tree based on reference map value
// - Adds to branch history set
// - Inserts into _eventCollection

void Model::addEventToTree(double x)
{
    BranchEvent* newEvent = newBranchEventWithRandomParameters(x);
            
    // Add the event to the branch history.
    // Always done after event is added to tree.
    newEvent->getEventNode()->getBranchHistory()->
        addEventToBranchHistory(newEvent);
                
    _eventCollection.insert(newEvent);
    forwardSetBranchHistories(newEvent);
    setMeanBranchParameters();
                            
    _lastEventModified = newEvent;
}


// TODO: ctr is not doing anything
BranchEvent* Model::chooseEventAtRandom()
{
    int numEvents = (int)_eventCollection.size();

    if (numEvents == 0) {
        log(Error) << "Number of events is zero.\n";
        std::exit(1);
    }

    int ctr = 0;
    double xx = _rng->uniformRv();
    int chosen = (int)(xx * (double)numEvents);

    EventSet::iterator sit = _eventCollection.begin();

    for (int i = 0; i < chosen; i++) {
        ++sit;
        ctr++;
    }

    return *sit;
}


void Model::eventLocalMove(void)
{
    eventMove(true);
}


void Model::eventGlobalMove(void)
{
    eventMove(false);
}


// If events are on tree: choose event at random,
// move locally (or globally) and forward set branch histories etc.
// Should also store previous event information to revert to previous

// If parameter local == true, does a local move;
// otherwise, it does a global move

void Model::eventMove(bool local)
{
    if (getNumberOfEvents() > 0) {
        // The event to be moved
        BranchEvent* chosenEvent = chooseEventAtRandom();

        // This is the event preceding the chosen event:
        // histories should be set forward from here.
        BranchEvent* previousEvent = chosenEvent->getEventNode()->
            getBranchHistory()->getLastEvent(chosenEvent);

        // Set this history variable in case move is rejected
        _lastEventModified = chosenEvent;

        chosenEvent->getEventNode()->getBranchHistory()->
            popEventOffBranchHistory(chosenEvent);

        if (local) {
            // Get step size for move
            double step = _rng->uniformRv(0, _scale) - 0.5 * _scale;
            chosenEvent->moveEventLocal(step);
        } else {
            chosenEvent->moveEventGlobal();
        }

        chosenEvent->getEventNode()->getBranchHistory()->
            addEventToBranchHistory(chosenEvent);

        // Get last event from the theEventNode, forward set its history.
        // Then go to the "moved" event and forward set its history.

        forwardSetBranchHistories(previousEvent);
        forwardSetBranchHistories(chosenEvent);
    }

    setMeanBranchParameters();
}


// Used to reset position of event if move is rejected

void Model::revertMovedEventToPrevious()
{
    // Get last event from position of event to be removed
    BranchEvent* newLastEvent = _lastEventModified->getEventNode()->
        getBranchHistory()->getLastEvent(_lastEventModified);

    // Pop event off its new position
    _lastEventModified->getEventNode()->getBranchHistory()->
        popEventOffBranchHistory(_lastEventModified);

    // Reset nodeptr, reset mapTime
    _lastEventModified->revertOldMapPosition();

    // Now reset forward from _lastEventModified (new position)
    // and from newLastEvent, which holds 'last' event before old position
    _lastEventModified->getEventNode()->getBranchHistory()->
        addEventToBranchHistory(_lastEventModified);

    // Forward set from new position
    forwardSetBranchHistories(newLastEvent);

    // Forward set from event immediately rootwards from previous position
    forwardSetBranchHistories(_lastEventModified);

    // Set _lastEventModified to NULL because it has already been reset.
    // Future implementations should check whether this is NULL
    // before attempting to use it to set event

    _lastEventModified = NULL;

    setMeanBranchParameters();
}


// Recursively count the number of events in the branch histories
int Model::countEventsInBranchHistory(Node* p)
{
    int count = p->getBranchHistory()->getNumberOfBranchEvents();

    if (p->getLfDesc() != NULL) {
        count += countEventsInBranchHistory(p->getLfDesc());
    }

    if (p->getRtDesc() != NULL){
        count += countEventsInBranchHistory(p->getRtDesc());
    }

    return count;
}
