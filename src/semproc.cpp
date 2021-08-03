////////////////////////////////////////////////////////////////////////////////
//                                                                            //
//  This file is part of ModelBlocks. Copyright 2009, ModelBlocks developers. //
//                                                                            //
//  ModelBlocks is free software: you can redistribute it and/or modify       //
//  it under the terms of the GNU General Public License as published by      //
//  the Free Software Foundation, either version 3 of the License, or         //
//  (at your option) any later version.                                       //
//                                                                            //
//  ModelBlocks is distributed in the hope that it will be useful,            //
//  but WITHOUT ANY WARRANTY; without even the implied warranty of            //
//  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the             //
//  GNU General Public License for more details.                              //
//                                                                            //
//  You should have received a copy of the GNU General Public License         //
//  along with ModelBlocks.  If not, see <http://www.gnu.org/licenses/>.      //
//                                                                            //
////////////////////////////////////////////////////////////////////////////////

#define ARMA_64BIT_WORD
#include <iostream>
#include <fstream>
#include <list>
#include <thread>
#include <mutex>
#include <chrono>
using namespace std;
#include <armadillo>
using namespace arma;
#define DONT_USE_UNMAPPABLE_TUPLES
#include <nl-randvar.h>
#include <nl-string.h>
#include <Delimited.hpp>
bool STORESTATE_TYPE = true;
bool STORESTATE_CHATTY = false;
uint FEATCONFIG = 0;
#include <StoreState.hpp>
#include <SemProcModels.hpp>
#include <Beam.hpp>
int COREF_WINDOW = 0;
bool ABLATE_UNARY = false;
bool NO_ENTITY_BLOCKING = false;

#define SERIAL_IO

////////////////////////////////////////////////////////////////////////////////

char psSpcColonSpc[]  = " : ";
char psSpcEqualsSpc[] = " = ";

////////////////////////////////////////////////////////////////////////////////

uint BEAM_WIDTH      = 1000;
uint VERBOSE         = 0;
uint OUTPUT_MEASURES = true;

////////////////////////////////////////////////////////////////////////////////

class Trellis : public vector<Beam<HiddState>> {
  public:
    Trellis ( ) : vector<Beam<HiddState>>() { reserve(100); }
    Beam<HiddState>& operator[] ( uint i ) { if ( i==size() ) emplace_back(BEAM_WIDTH); return vector<Beam<HiddState>>::operator[](i); }
    void setMostLikelySequence ( DelimitedList<psX,BeamElement<HiddState>,psLine,psX>& lbe, const JModel& jm ) {
      static StoreState ssLongFail( cFail, cFail );
      lbe.clear(); if( back().size()>0 ) lbe.push_front( *back().begin() );
      if( lbe.size()>0 ) for( int t=size()-2; t>=0; t-- ) lbe.push_front( lbe.front().getBack() );
      if( lbe.size()>0 ) lbe.emplace_back( BeamElement<HiddState>() );
      cerr << "lbe.size(): " << lbe.size() << endl;
      // If parse fails...
      if( lbe.size()==0 ) {
        cerr << "parse failed (lbe.size() = 0) " << "trellis size(): " << size() << endl;
        // Print a right branching structure...
        for( int t=size()-2; t>=0; t-- ) { 
          lbe.push_front( BeamElement<HiddState>( ProbBack<HiddState>(), HiddState( Sign(hvBot,cFail,S_A), 1, EVar::eNil, K::kBot, jm.getResponse1(), ssLongFail ) ) ); // fork and join
        }
        lbe.front() = BeamElement<HiddState>( ProbBack<HiddState>(), HiddState( Sign(hvBot,cFail,S_A), 1, EVar::eNil, K::kBot, jm.getResponse0(), ssLongFail ) );       // front: fork no-join
        lbe.back( ) = BeamElement<HiddState>( ProbBack<HiddState>(), HiddState( Sign(hvBot,cFail,S_A), 0, EVar::eNil, K::kBot, jm.getResponse1(), StoreState() ) );     // back: join no-fork
        if( size()==2 ) {  //special case if single word, fork and join
          lbe.front() = BeamElement<HiddState>( ProbBack<HiddState>(), HiddState( Sign(hvBot,cFail,S_A), 1, EVar::eNil, K::kBot, jm.getResponse1(), StoreState() ) );   // unary case: fork and join
        }
        lbe.push_front( BeamElement<HiddState>( ProbBack<HiddState>(), HiddState( Sign(hvBot,cFail,S_A), 0, EVar::eNil, K::kBot, jm.getResponse0(), StoreState() ) ) ); // no-fork, no-join?
        lbe.emplace_back( BeamElement<HiddState>() );
        cerr << "Parse failed." << endl;
      }
      // For each element of MLE after first dummy element...
      int u=-1; for( auto& be : lbe ) if( ++u>0 and u<int(size()) ) {
        // Calc surprisal as diff in exp of beam totals of successive elements, minus constant...
        double probPrevTot = 0.0;
        double probCurrTot = 0.0;
        for( auto& beP : at(u-1) ) probPrevTot += exp( beP.getProb() - at(u-1).begin()->getProb() );
        for( auto& beC : at(u  ) ) probCurrTot += exp( beC.getProb() - at(u-1).begin()->getProb() ); 
        be.setProb() = log2(probPrevTot) - log2(probCurrTot);     // store surp into prob field of beam item
      }
    }
};

const BeamElement<HiddState>* getNextAntecedent (const BeamElement<HiddState>* antPtr) {
    int i = antPtr->getHidd().getI(); //offset to antecedent timestep
    i = abs(i);
    for (; i != 0; i--) {
      antPtr = &antPtr->getBack();
    }
    return antPtr;
  }

W getHistWord ( const BeamElement<HiddState>* antPtr ) {
  W histword = W(""); //default case - no unk, no coreference, histword is ""
  if (antPtr->getHidd().getForkK().isUnk()) { //unk case - unk, histword is word, short-circuits inheritance case
    histword = antPtr->getHidd().getWord();
    return histword;  
  } 
  for ( ; (antPtr->getHidd().getI() != 0) ; antPtr = getNextAntecedent(antPtr) ) { //inheritance case - break upon finding most recent unk in antecedent chain, word is that unk's word
    if (antPtr->getHidd().getForkK().isUnk()) {
      histword = antPtr->getHidd().getWord(); //at most recent unk, get observed word and return
      break;
    }
  }
  return histword;
}

////////////////////////////////////////////////////////////////////////////////

int main ( int nArgs, char* argv[] ) {

  uint numThreads = 1;

  // Define model structures...
  EMat                          matEmutable;
  OFunc                         funcOmutable;
  NModel                        modNmutable;
  FModel                        modFmutable;
  PModel                        modPmutable;
  WModel                        modWmutable;
  JModel                        modJmutable;
  AModel                        modAmutable;
  BModel                        modBmutable;

  { // Define model lists...
    list<DelimitedTrip<psX,WPredictor,psSpcColonSpc,W,psSpcEqualsSpc,Delimited<double>,psX>> lW;

    // For each command-line flag or model file...
    for ( int a=1; a<nArgs; a++ ) {
      if      ( 0==strcmp(argv[a],"-v") ) VERBOSE = 1;
      else if ( 0==strcmp(argv[a],"-V") ) VERBOSE = 2;
      else if ( 0==strncmp(argv[a],"-p",2) ) numThreads = atoi(argv[a]+2);
      else if ( 0==strncmp(argv[a],"-b",2) ) BEAM_WIDTH = atoi(argv[a]+2);
      else if ( 0==strncmp(argv[a],"-f",2) ) FEATCONFIG = atoi(argv[a]+2);
      else {
        cerr << "Loading model " << argv[a] << "..." << endl;
        // Open file...
        ifstream fin (argv[a], ios::in );
        // Read model lists...
        int linenum = 0;
        while ( fin && EOF!=fin.peek() ) {
          if      ( fin.peek()=='E' ) matEmutable = EMat( fin );
          else if ( fin.peek()=='O' ) funcOmutable = OFunc( fin );
          else if ( fin.peek()=='N' ) modNmutable = NModel( fin );
          else if ( fin.peek()=='F' ) modFmutable = FModel( fin );
          else if ( fin.peek()=='P' ) modPmutable = PModel( fin );
          else if ( fin.peek()=='W' ) modWmutable = WModel( fin );
          else if ( fin.peek()=='J' ) modJmutable = JModel( fin );
          else if ( fin.peek()=='A' ) modAmutable = AModel( fin );
          else if ( fin.peek()=='B' ) modBmutable = BModel( fin );
          else {
            Delimited<string> sOffSpec;
            fin >> sOffSpec >> "\n";
            cerr << "WARNING: skipping off-spec input line: '" << sOffSpec << "'" << endl;
          } 
          if ( ++linenum%1000000==0 ) cerr << "  " << linenum << " items loaded..." << endl;
        }
        cerr << "Model " << argv[a] << " loaded." << endl;
      }
    } //closes for int a=1
  } //closes define model lists

  const EMat&   matE  = matEmutable;
  const OFunc&  funcO = funcOmutable;
  const NModel& modN  = modNmutable;
  const FModel& modF  = modFmutable;
  const PModel& modP  = modPmutable;
  const WModel& modW  = modWmutable;
  const JModel& modJ  = modJmutable;
  const AModel& modA  = modAmutable;
  const BModel& modB  = modBmutable;

  cerr<<"Models ready."<<endl;

  mutex mutexMLSList;
  vector<thread> vtWorkers;  vtWorkers.reserve( numThreads );

  if( OUTPUT_MEASURES ) cout << "word surprisal" << endl;

#ifdef SERIAL_IO
  // List of articles, which are pairs of lists of lists of words and lists of lists of hidd states...
  list< pair< DelimitedList<psX,DelimitedList<psX,ObsWord,psSpace,psX>,psLine,psX>, DelimitedList<psX,DelimitedList<psX,BeamElement<HiddState>,psLine,psX>,psLine,psX> > > corpus;
  // Read in list of articles...
  while( cin.peek() != EOF ) {
    if( cin.peek() == '!' ) cin >> "!ARTICLE\n";
    corpus.emplace_back();
    while( cin.peek() != '!' && cin.peek() != EOF )  cin >> *corpus.back().first.emplace( corpus.back().first.end() ) >> "\n";
    cerr<<"I read an article with " << corpus.back().first.size() << " sentences." << endl;
  }
  // Pointers to 
  auto iartNextToProc = corpus.begin();
  auto iartNextToDump = corpus.begin();
#else
  uint linenum = 0;
  // For each line in stdin...
  list<list<DelimitedList<psX,ObsWord,psSpace,psX>>> articles; //list of list of sents. each list of sents is an article.
  list<list<DelimitedList<psX,BeamElement<HiddState>,psLine,psX>>> articleMLSs; //list of MLSs
#endif

  // loop over threads (each thread gets an article)
  for( uint numtglobal=0; numtglobal<numThreads; numtglobal++ ) vtWorkers.push_back( thread( [&corpus,&iartNextToProc,&iartNextToDump,/*&articleMLSs,&articles,&linenum,*/&mutexMLSList,numThreads,&matE,&funcO,&modN,&modF,&modP,&modW,&modJ,&modA,&modB] (uint numt) {

    auto tpLastReport = chrono::high_resolution_clock::now();  // clock for thread heartbeats
    // WModel-related maps for each thread
    WModel::WWPPMap mapWWPP;
    WModel::XPMap mapXP;
    WModel::MPMap mapMP;

    // Loop over articles...
    while( true ) {

#ifdef SERIAL_IO
      decltype(corpus)::iterator iart;
      { lock_guard<mutex> guard( mutexMLSList );
        if( iartNextToProc == corpus.end() ) break;
        iart = iartNextToProc++;
      }
      const auto& sents = iart->first;
      auto&       MLSs  = iart->second;

      int currline = 0;
#else
      // Read in your worker thread's article in this lock block
      mutexMLSList.lock( );
      if( not ( cin && EOF!=cin.peek() ) ) { mutexMLSList.unlock(); break; }

      uint currline = linenum;
      articles.emplace_back();
      auto& sents = articles.back(); //a specific article becomes the thread's sents //returns reference
      articleMLSs.emplace_back();
      auto& MLSs = articleMLSs.back();

      DelimitedList<psX,ObsWord,psSpace,psX> articleDelim; // !article should be consumed between sentence reads
      //loop over sentences in an article
      cin >> articleDelim >> "\n"; //consume !ARTICLE

      while (cin.peek()!='!' and cin.peek()!=EOF) {
        // Read sentence...
        linenum++; //updates linenum for when handing off to other thread
        DelimitedList<psX,ObsWord,psSpace,psX> lwSent; // init input list for each iteration - otherwise cin just keeps appending to existing lwSent
        cin >> lwSent >> "\n";
        sents.emplace_back( lwSent );
      }
      mutexMLSList.unlock();
#endif

      if ( numThreads == 1 ) cerr << "#" << currline;

      DelimitedList<psX,BeamElement<HiddState>,psLine,psX> lbeWholeArticle;
      lbeWholeArticle.emplace_back(); //create null beamElement at start of article

      for (auto& lwSent : sents) {
        currline++;

#ifdef SERIAL_IO
#else
        // Add mls to list...
        MLSs.emplace_back( ); //establish placeholder for mls for this specific sentence
        auto& mls = MLSs.back();
#endif

        Trellis   beams;  // sequence of beams - each beam is hypotheses at a given timestep
        uint      t=0;    // time step

        // Allocate space in beams to avoid reallocation...
        // Create initial beam element...
        lbeWholeArticle.back().setProb() = 0.0;
        beams[0].tryAdd( lbeWholeArticle.back().getHidd(), lbeWholeArticle.back().getProbBack() );

        // For each word...
        for ( auto& w_t : lwSent ) {
          if ( VERBOSE ) cout << "WORD:" << w_t << endl;

          // Create beam for current time step...
          beams[++t].clear();

          WModel::WPPMap mapWPP;
          modW.calcPredictorLikelihoods(w_t, mapWWPP, mapXP, mapMP, mapWPP);
          mapWWPP.try_emplace(w_t, mapWPP);

          // For each hypothesized storestate at previous time step...
          for( const BeamElement<HiddState>& be_tdec1 : beams[t-1] ) { //beams[t-1] is a Beam<ProbBack,BeamElement>, so be_tdec1 is a beam item, which is a pair<ProbBack,BeamElement>. first.first is the prob in the probback, and second is the beamelement, which is a sextuple of <sign, f, e, k, j, q>
            double            lgpr_tdec1 = be_tdec1.getProb(); // logprob of prev storestate
            const StoreState& q_tdec1    = be_tdec1.getHidd().sixth();  // prev storestate
            if( VERBOSE>1 ) cout << "  from (" << be_tdec1.getHidd() << ")" << endl;
            const ProbBack<HiddState> pbDummy = ProbBack<HiddState>(0.0, be_tdec1); //dummy element for most recent timestep
            const HiddState hsDummy = HiddState(Sign(/*hvTop,CVar(),S()*/),F(),EVar(),K(),JResponse(),StoreState(),0 ); //dummy hidden state with kTop semantics
            const BeamElement<HiddState> beDummy = BeamElement<HiddState>(pbDummy, hsDummy); //at timestep t, represents null antecedent
            const BeamElement<HiddState>* pbeAnt = &beDummy;
            double ndenom = 0.0;

            vector<int> excludedIndices; //initialize blocking list

            //denominator loop over candidate antecedents
            for ( int tAnt = t; (&pbeAnt->getBack() != &BeamElement<HiddState>::beStableDummy) && (int(t-tAnt)<=COREF_WINDOW); tAnt--, pbeAnt = &pbeAnt->getBack()) {
              if ( pbeAnt->getHidd().getI() != 0 ) {
                if (VERBOSE > 1) cout << "    adding index to exclude for blocking: " << tAnt+pbeAnt->getHidd().getI() << " pbeAnt...get(): " << pbeAnt->getHidd().getI() << endl;
                excludedIndices.push_back(tAnt+pbeAnt->getHidd().getI()); //add excluded index if there's a non-null coref decision
              }
              if (NO_ENTITY_BLOCKING == false) {
                if (std::find(excludedIndices.begin(), excludedIndices.end(), tAnt) != excludedIndices.end()){
                  continue; //skip excluded indices
                }
              }
              bool corefON = (tAnt==int(t)) ? 0 : 1;
              NPredictorVec npv( modN, pbeAnt->getHidd().getPrtrm(), corefON, t - tAnt, q_tdec1, ABLATE_UNARY );
              arma::vec nlogresponses = modN.calcLogResponses( npv );
              ndenom += exp( nlogresponses(1) - nlogresponses(0) );
            } //closes for tAnt

            pbeAnt = &beDummy; //reset pbiAnt pointer after calculating denominator

            for ( int tAnt = t; (&pbeAnt->getBack() != &BeamElement<HiddState>::beStableDummy) && (int(t-tAnt)<=COREF_WINDOW); tAnt--, pbeAnt = &pbeAnt->getBack()) { //numerator, iterate over candidate antecedent ks, following trellis backpointers.
              //block indices as read from previous storestate's excludedIndices
              if (std::find(excludedIndices.begin(), excludedIndices.end(), tAnt) != excludedIndices.end()){
                continue;
              }

              const HVec& hvAnt = pbeAnt->getHidd().getPrtrm().getHVec();

              //Calculate antecedent N model predictors
              bool corefON = (tAnt==int(t)) ? 0 : 1;
              NPredictorVec npv( modN, pbeAnt->getHidd().getPrtrm(), corefON, t - tAnt, q_tdec1, ABLATE_UNARY );
              if (VERBOSE>1) { cout << "    " << pair<const NModel&, const NPredictorVec&>(modN,npv) << endl; } //npv.printOut(cout); }
              arma::vec nlogresponses = modN.calcLogResponses( npv );

              double numerator = exp( nlogresponses(1) - nlogresponses(0) );
              double nprob = numerator / ndenom;

              if ( VERBOSE>1 ) cout << "    N ... : 1 = " << numerator << "/" << ndenom << "=" << nprob << "  tAnt: " << (t - tAnt) << endl;

              if( beams[t].size()<BEAM_WIDTH || lgpr_tdec1 + log(nprob) > beams[t].rbegin()->getProb() ) {
                FPredictorVec lfpredictors( modF, hvAnt, not corefON, q_tdec1 );
                arma::vec fresponses = modF.calcResponses( lfpredictors );

                for ( auto& ektpr_p_t : mapWPP ) { //ektpr_p_t is a pair of (Wpredictor, prob)

                  if( beams[t].size()<BEAM_WIDTH || lgpr_tdec1 + log(nprob) + log(ektpr_p_t.second) > beams[t].rbegin()->getProb() ) {
                    EVar  e_p_t       = ektpr_p_t.first.first();
                    K     k_p_t       = (FEATCONFIG & 8 && ektpr_p_t.first.second().getString()[2]!='y') ? K::kBot : ektpr_p_t.first.second();   // context of current preterminal
                    CVar  c_p_t       = ektpr_p_t.first.third();                               // label of current preterminal
                    double probwgivkl = ektpr_p_t.second;                                     // probability of current word given current preterminal

                    if ( VERBOSE>1 ) cout << "     W " << e_p_t << " " << k_p_t << " " << c_p_t << " : " << w_t << " = " << probwgivkl << endl;

                    // For each possible no-fork or fork decision...
                    for ( auto& f : {0,1} ) if ( q_tdec1.size() > 0 or f > 0 ) {
                      if( modF.getResponseIndex(f,e_p_t,k_p_t) == uint(-1) ) continue;
                      double probFork = fresponses( modF.getResponseIndex(f,e_p_t,k_p_t) );
                      if ( VERBOSE>1 ) cout << "      F ... : " << f << " " << e_p_t << " " << k_p_t << " = " << probFork << endl;

                      // Thread heartbeat (to diagnose thread death)...
                      if( chrono::high_resolution_clock::now() > tpLastReport + chrono::minutes(1) ) {
                        tpLastReport = chrono::high_resolution_clock::now();
                        lock_guard<mutex> guard( mutexMLSList );
                        cerr << "WORKER " << numt << ": SENT " << currline << " WORD " << t << " FROM " << be_tdec1.getHidd() << " PRED " << ektpr_p_t.first << ektpr_p_t.second << endl;
                      } //closes if chrono

                      // If preterminal prob is nonzero...
                      PPredictorVec ppredictor( f, e_p_t, k_p_t, q_tdec1 );
                      if ( VERBOSE>1 ) cout << "       P " << ppredictor << " : " << c_p_t << "...?" << endl;
                      if ( modP.end()!=modP.find(ppredictor) && modP.find(ppredictor)->second.end()!=modP.find(ppredictor)->second.find(c_p_t) ) {

                        if ( VERBOSE>1 ) cout << "       P " << ppredictor << " : " << c_p_t << " = " << modP.find(ppredictor)->second.find(c_p_t)->second << endl;

                        // Calc probability for fork phase...
                        double probFPW = probFork * modP.find(ppredictor)->second.find(c_p_t)->second * probwgivkl;
                        if ( VERBOSE>1 ) cout << "       f: f" << f << "&" << e_p_t << "&" << k_p_t << " " << probFork << " * " << modP.find(ppredictor)->second.find(c_p_t)->second << " * " << probwgivkl << " = " << probFPW << endl;

                        StoreState qPretrm( q_tdec1, hvAnt, e_p_t, k_p_t, c_p_t, matE, funcO );
                        const Sign& aPretrm = qPretrm.getApex();
                        if( VERBOSE>1 ) cout << "       qPretrm="    << qPretrm    << endl;
                        StoreState qTermPhase( qPretrm, f );
                        const Sign& aLchild = qTermPhase.getApex();
                        if( VERBOSE>1 ) cout << "       qTermPhase=" << qTermPhase << endl;

                        JPredictorVec ljpredictors( modJ, f, e_p_t, aLchild, qTermPhase );  // q_tdec1.calcJoinPredictors( ljpredictors, f, e_p_t, aLchild, false ); // predictors for join
                        arma::vec jresponses = modJ.calcResponses( ljpredictors );

                        // For each possible no-join or join decision, and operator decisions...
                        for( unsigned int jresponse=0; jresponse<jresponses.size(); jresponse++ ) {  //JResponse jresponse; jresponse<JResponse::getDomain().getSize(); ++jresponse ) {
                          if( beams[t].size()<BEAM_WIDTH || lgpr_tdec1 + log(nprob) + log(probFPW) + log(jresponses[jresponse]/* /jnorm */) > beams[t].rbegin()->getProb() ) {
                            J    j   = modJ.getJEOO( jresponse ).first();  //.getJoin();
                            EVar e   = modJ.getJEOO( jresponse ).second(); //.getE();
                            O    opL = modJ.getJEOO( jresponse ).third();  //.getLOp();
                            O    opR = modJ.getJEOO( jresponse ).fourth(); //.getROp();
                            double probJoin = jresponses[jresponse]; //  / jnorm;
                            if ( VERBOSE>1 ) cout << "        J ... " << " : " << modJ.getJEOO(jresponse) << " = " << probJoin << endl;

                            // For each possible apex category label...
                            APredictorVec apredictor( f, j, e_p_t, e, opL, aLchild, qTermPhase );  // save apredictor for use in prob calc
                            if ( VERBOSE>1 ) cout << "         A " << apredictor << "..." << endl;
                            if ( modA.end()!=modA.find(apredictor) )
                              for ( auto& cpA : modA.find(apredictor)->second ) {
                                if( beams[t].size()<BEAM_WIDTH || lgpr_tdec1 + log(nprob) + log(probFPW) + log(probJoin) + log(cpA.second) > beams[t].rbegin()->getProb() ) {

                                  if ( VERBOSE>1 ) cout << "         A " << apredictor << " : " << cpA.first << " = " << cpA.second << endl;

                                  // For each possible base category label...
                                  BPredictorVec bpredictor( f, j, e_p_t, e, opL, opR, cpA.first, aLchild, qTermPhase );  // bpredictor for prob calc
                                  if ( VERBOSE>1 ) cout << "          B " << bpredictor << "..." << endl;
                                  if ( modB.end()!=modB.find(bpredictor) )
                                    for ( auto& cpB : modB.find(bpredictor)->second ) {
                                      if ( VERBOSE>1 ) cout << "          B " << bpredictor << " : " << cpB.first << " = " << cpB.second << endl;
                                      //                            lock_guard<mutex> guard( mutexBeam );
                                      if( beams[t].size()<BEAM_WIDTH || lgpr_tdec1 + log(nprob) + log(probFPW) + log(probJoin) + log(cpA.second) + log(cpB.second) > beams[t].rbegin()->getProb() ) {

                                        // Thread heartbeat (to diagnose thread death)...
                                        if( chrono::high_resolution_clock::now() > tpLastReport + chrono::minutes(1) ) {
                                          tpLastReport = chrono::high_resolution_clock::now();
                                          lock_guard<mutex> guard( mutexMLSList );
                                          cerr << "WORKER " << numt << ": SENT " << currline << " WORD " << t << " FROM " << be_tdec1.getHidd() << " PRED " << ektpr_p_t.first << ektpr_p_t.second << " JRESP " << modJ.getJEOO(jresponse) << " A " << cpA.first << " B " << cpB.first << endl;
                                        } //closes if chrono
                                        // Calculate probability and storestate and add to beam...
                                        StoreState ss( qTermPhase, j, e, opL, opR, cpA.first, cpB.first );
                                        if( (t<lwSent.size() && ss.size()>0) || (t==lwSent.size() && ss.size()==0) ) {
                                          beams[t].tryAdd( HiddState( aPretrm, f,e_p_t,k_p_t, jresponse, ss, tAnt-t, w_t ), ProbBack<HiddState>( lgpr_tdec1 + log(nprob) + log(probFPW) + log(probJoin) + log(cpA.second) + log(cpB.second), be_tdec1 ) );
                                          if( VERBOSE>1 ) cout << "                send (" << be_tdec1.getHidd() << ") to (" << ss << ") with "
                                            << (lgpr_tdec1 + log(nprob) + log(probFPW) + log(probJoin) + log(cpA.second) + log(cpB.second)) << endl;
                                        } //closes if ( (t<lwSent
                                      } //closes if beams[t]
                                    } //closes for cpB
                                } //closes if beams[t]
                              } //closes for cpA
                          } //closes if beams[t]
                        } //closes for jresponse
                      } //closes if modP.end()
                    } //closes for f in {0,1}
                  } //closes if beams[t]
                } //closes for ektpr_p_t
              } // short-circuit bad nprob
            } //closes for tAnt (second antecedent loop)
          } //closes be_tdec1

          // Write output...
          if ( numThreads == 1 ) cerr << " (" << beams[t].size() << ")";
          if ( VERBOSE ) { //cout << beams[t] << endl;
            cout << "BEAM" << endl;
            for( auto& be : beams[t] )
              cout << be.getProb() << " " << be.getHidd().first() << " f" << be.getHidd().second() << "&" << be.getHidd().third() << "&" << be.getHidd().fourth() << " j" << modJ.getJEOO(be.getHidd().fifth()) << " " << be.getHidd().sixth() << " " << be.getHidd().seventh() << " me: " << &be << " myback: " << &be.getBack() << endl; //tokdecs output is: WORD HIDDSTATE PROB
          }
          { lock_guard<mutex> guard( mutexMLSList );
            cerr << "WORKER " << numt << ": SENT " << currline << " WORD " << t << endl;
          } //closes lock_guard
        } //closes for w lwSent  
        if ( numThreads == 1 ) cerr << endl;
        if ( VERBOSE ) cout << "MLS" << endl;

        { lock_guard<mutex> guard( mutexMLSList );
#ifdef SERIAL_IO
          auto& mls = *MLSs.emplace( MLSs.end() ); //establish placeholder for mls for this specific sentence
#else
#endif 
          if( numThreads > 1 ) cerr << "Finished line " << currline << " (" << beams[t].size() << ")..." << endl;
          beams.setMostLikelySequence( mls, modJ );
          mls.pop_back(); //remove dummy element before adding to lbe
          lbeWholeArticle.insert(lbeWholeArticle.end(), next(mls.begin()), mls.end()); //insert mls at end of lbe
          for (auto it = lbeWholeArticle.begin(); it != lbeWholeArticle.end(); it++) {
            if ( it != lbeWholeArticle.begin() ) {
              it->setBack(*prev(it));
            }
          }
        }
      } //close loop lwSent over sents

      { lock_guard<mutex> guard( mutexMLSList );
#ifdef SERIAL_IO
        while( iartNextToDump != corpus.end() and iartNextToDump->first.size() == iartNextToDump->second.size() ) {
          auto isent = iartNextToDump->first.begin();  // Iterator over sentences.
          auto imls  = iartNextToDump->second.begin(); // Iterator over most likely sequences.
          for( ;  isent != iartNextToDump->first.end() and imls != iartNextToDump->second.end();  isent++, imls++ ) {
            auto iw  = isent->begin();         // Iterator over words.
            auto ibe = next( imls->begin() );  // Iterator over mls elements.
            for( ;  iw != isent->end() and ibe != imls->end();  ibe++, iw++ )
              cout << *iw << " " << ibe->getProb() << endl; // totsurp output is: WORD SURP
          }
          iartNextToDump++;
        }
#else
        while( articleMLSs.size()>0 && articleMLSs.front().size()>0 && articleMLSs.front().size()==articles.front().size() ) { 
          int u=1; 
          auto ibe=next(articleMLSs.front().front().begin());  //iterator over beam elements?
          auto iw=articles.front().front().begin() ; //iterator over words
          for( ; (ibe != articleMLSs.front().front().end()) && (iw != articles.front().front().end()); ibe++, iw++, u++ ) {
            cout << *iw << " " << ibe->getHidd().first() << " f" << ibe->getHidd().second() << "&" << ibe->getHidd().third() << "&" << ibe->getHidd().fourth() << " j" << modJ.getJEOO(ibe->getHidd().fifth()) << " " << ibe->getHidd().sixth() << " " << ibe->getHidd().seventh() << " " << ibe->getProb(); //tokdecs output is: WORD HIDDSTATE PROB
            cout << endl;
          } //closes for ibe!=mls.end
          articleMLSs.front().pop_front(); //pop (mls of) first sentence of first article
          articles.front().pop_front(); //pop first sentence of first article
          if (articles.front().size() == 0) {  //if article is empty then pop article
            articleMLSs.pop_front(); 
            articles.pop_front();
          } 
        } //closes while articleMLSs
#endif
      } //closes lock guard for print loop  
    } //closes while(True)
  }, numtglobal )); //brace closes for numtglobal

  for( auto& w : vtWorkers ) w.join();

} //closes int main
