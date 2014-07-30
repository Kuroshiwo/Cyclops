/*
 * CyclicCoordinateDescent.cpp
 *
 *  Created on: May-June, 2010
 *      Author: msuchard
 */

#include <iostream>
#include <fstream>
#include <sstream>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <map>
#include <time.h>
#include <set>

#include "CyclicCoordinateDescent.h"
#include "io/InputReader.h"
#include "Iterators.h"

//#ifdef MY_RCPP_FLAG
//	#include <R.h>
//#else
//	void Rprintf(...) {
//		// Do nothing
//	}
//#endif

#ifndef MY_RCPP_FLAG
#define PI	3.14159265358979323851280895940618620443274267017841339111328125
#else
//#include "Rcpp.h"
#endif

namespace bsccs {

using namespace std;

void compareIntVector(int* vec0, int* vec1, int dim, const char* name) {
	for (int i = 0; i < dim; i++) {
		if (vec0[i] != vec1[i]) {
			cerr << "Error at " << name << "[" << i << "]: ";
			cerr << vec0[i] << " != " << vec1[i] << endl;
			exit(0);
		}
	}
}

CyclicCoordinateDescent::CyclicCoordinateDescent(
			ModelData* reader,
			AbstractModelSpecifics& specifics
//			ModelSpecifics<DefaultModel>& specifics
		) : modelSpecifics(specifics) {
	N = reader->getNumberOfPatients();
	K = reader->getNumberOfRows();
	J = reader->getNumberOfColumns();
	
	hXI = reader;
	hY = reader->getYVector(); // TODO Delegate all data to ModelSpecifics
	hOffs = reader->getOffsetVector();
//	hNEvents = NULL;
	hPid = reader->getPidVector();

	conditionId = reader->getConditionId();
	denomNullValue = static_cast<real>(0.0);

	updateCount = 0;
	likelihoodCount = 0;
	noiseLevel = NOISY;

	init(reader->getHasOffsetCovariate());
}

CyclicCoordinateDescent::~CyclicCoordinateDescent(void) {

//	free(hPid);
//	free(hNEvents);
//	free(hY);
//	free(hOffs);
	
	free(hXBeta);
	free(hXBetaSave);
	free(hDelta);
	
#ifdef TEST_ROW_INDEX
	for (int j = 0; j < J; ++j) {
		if (hXColumnRowIndicators[j]) {
			free(hXColumnRowIndicators[j]);
		}
	}
	free(hXColumnRowIndicators);
#endif

	free(hXjY);
	free(offsExpXBeta);
	free(xOffsExpXBeta);
//	free(denomPid);  // Nested in numerPid allocation
	free(numerPid);
//	free(t1);
	
#ifdef NO_FUSE
	free(wPid);
#endif
	
	if (hWeights) {
		free(hWeights);
	}

#ifdef SPARSE_PRODUCT
	for (std::vector<std::vector<int>* >::iterator it = sparseIndices.begin();
			it != sparseIndices.end(); ++it) {
		if (*it) {
			delete *it;
		}
	}
#endif
}

void CyclicCoordinateDescent::setNoiseLevel(NoiseLevels noise) {
	noiseLevel = noise;
}

string CyclicCoordinateDescent::getPriorInfo() {
#ifdef MY_RCPP_FLAG
	return "prior"; // Rcpp error with stringstream
#else
	stringstream priorInfo;
	if (priorType == NONE) {
		priorInfo << "None(";
	} else if (priorType == LAPLACE) {
		priorInfo << "Laplace(";
		priorInfo << lambda;
	} else if (priorType == NORMAL) {
		priorInfo << "Normal(";
		priorInfo << sigma2Beta;
	}
	priorInfo << ")";
	return priorInfo.str();
#endif
}

void CyclicCoordinateDescent::resetBounds() {
	for (int j = 0; j < J; j++) {
		hDelta[j] = 2.0;
	}
}

void CyclicCoordinateDescent::init(bool offset) {
	
	// Set parameters and statistics space
	hDelta = (real*) malloc(J * sizeof(real));
	hBeta = (real*) calloc(J, sizeof(real)); // Fixed starting state
	hXBeta = (real*) calloc(K, sizeof(real));
	hXBetaSave = (real*) calloc(K, sizeof(real));
	fixBeta.resize(J);

	// Set prior
	priorType = LAPLACE;
	sigma2Beta = 1000;
	lambda = sqrt(2.0/20.0);
	
	// Recode patient ids  TODO Delegate to grouped model
	int currentNewId = 0;
	int currentOldId = hPid[0];
	
	for(int i = 0; i < K; i++) {
		if (hPid[i] != currentOldId) {			
			currentOldId = hPid[i];
			currentNewId++;
		}
		hPid[i] = currentNewId;
	}
		
	// Init temporary variables
	offsExpXBeta = (real*) malloc(sizeof(real) * K);
	xOffsExpXBeta = (real*) malloc(sizeof(real) * K);

	// Put numer, numer2 and denom in single memory block, with first entries on 16-word boundary
	int alignedLength = getAlignedLength(N);
	numerPid = (real*) malloc(sizeof(real) * 3 * alignedLength);
	denomPid = numerPid + alignedLength; // Nested in denomPid allocation
	numerPid2 = numerPid + 2 * alignedLength;

//	hNEvents = (int*) malloc(sizeof(int) * N);
	hXjY = (real*) malloc(sizeof(real) * J);
	hWeights = NULL;
	
#ifdef NO_FUSE
	wPid = (real*) malloc(sizeof(real) * alignedLength);
#endif

	// TODO Suspect below is not necessary for non-grouped data.
	// If true, then fill with pointers to CompressedDataColumn and do not delete in destructor
	for (int j = 0; j < J; ++j) {
		if (hXI->getFormatType(j) == DENSE) {
			sparseIndices.push_back(NULL);
		} else {
			std::set<int> unique;
			const int n = hXI->getNumberOfEntries(j);
			const int* indicators = hXI->getCompressedColumnVector(j);
			for (int j = 0; j < n; j++) { // Loop through non-zero entries only
				const int k = indicators[j];
				const int i = hPid[k];
				unique.insert(i);
			}
			std::vector<int>* indices = new std::vector<int>(unique.begin(),
					unique.end());
			sparseIndices.push_back(indices);
		}
	}

	useCrossValidation = false;
	validWeights = false;
	sufficientStatisticsKnown = false;
	fisherInformationKnown = false;
	if (offset) {
		hBeta[0] = static_cast<real>(1);
		fixBeta[0] = true;
		xBetaKnown = false;
	} else {
		xBetaKnown = true; // all beta = 0 => xBeta = 0
	}
	doLogisticRegression = false;

#ifdef DEBUG	
#ifndef MY_RCPP_FLAG
	cerr << "Number of patients = " << N << endl;
	cerr << "Number of exposure levels = " << K << endl;
	cerr << "Number of drugs = " << J << endl;
#endif
#endif          

//#ifdef MY_RCPP_FLAG
//	Rprintf("Number of patients = %d\n", N);
//	Rprintf("Number of exposure levels = %d\n", K);
//	Rprintf("Number of drugs = %d\n", J);
//#endif
	modelSpecifics.initialize(N, K, J, hXI, numerPid, numerPid2, denomPid,
//			hNEvents,
			hXjY, &sparseIndices,
			hPid, offsExpXBeta,
			hXBeta, hOffs,
			hBeta,
			hY//,
//			hWeights
			);

//	int iN,
//	int iK,
//	CompressedDataMatrix* iXI,
//	real* iNumerPid,
//	real* iNumerPid2,
//	real* iDenomPid,
//	int* iNEvents,
//	real* iXjEta,
//	std::vector<std::vector<int>* > *iSparseIndices,
//	int* iPid,
//	real* iOffsExpXBeta,
//	real* iXBeta,
//	int* iOffs,
//	real* iBeta
}

int CyclicCoordinateDescent::getAlignedLength(int N) {
	return (N / 16) * 16 + (N % 16 == 0 ? 0 : 16);
}

void CyclicCoordinateDescent::computeNEvents() {
	modelSpecifics.setWeights(hWeights, useCrossValidation);
}

void CyclicCoordinateDescent::resetBeta(void) {
	for (int j = 0; j < J; j++) {
		hBeta[j] = 0.0;
	}
	for (int k = 0; k < K; k++) {
		hXBeta[k] = 0.0;
	}
	sufficientStatisticsKnown = false;
}

//void double getHessianComponent(int i, int j) {
//	return 0.0;
//}

//void getHessian(SparseMatrix& hessian) {
//
//}

void CyclicCoordinateDescent::logResults(const char* fileName, bool withASE) {

	ofstream outLog(fileName);
	if (!outLog) {
		cerr << "Unable to open log file: " << fileName << endl;
		exit(-1);
	}
	string sep(","); // TODO Make option

	outLog << "label"
			<< sep << "estimate"
			//<< sep << "score"
			;
	if (withASE) {
		outLog << sep << "ASE";
	}
	outLog << endl;

	for (int i = 0; i < J; i++) {		
		outLog << hXI->getColumn(i).getLabel()
//				<< sep << conditionId
				<< sep << hBeta[i];
		if (withASE) {
			double ASE = sqrt(getAsymptoticVariance(i,i));
			outLog << sep << ASE;
		}
		outLog << endl;
	}
	outLog.flush();
	outLog.close();
}

double CyclicCoordinateDescent::getPredictiveLogLikelihood(real* weights) {

	if (!xBetaKnown) {
		computeXBeta();
	}

	if (!sufficientStatisticsKnown) {
		computeRemainingStatistics(true, 0); // TODO Remove index????
	}

	getDenominators();

	return modelSpecifics.getPredictiveLogLikelihood(weights); // TODO Pass double
}

void CyclicCoordinateDescent::getPredictiveEstimates(real* y, real* weights) const {
	modelSpecifics.getPredictiveEstimates(y, weights);

//	printVector(y, K, cout);
}

int CyclicCoordinateDescent::getBetaSize(void) {
	return J;
}

int CyclicCoordinateDescent::getPredictionSize(void) const {
	return K;
}

real CyclicCoordinateDescent::getBeta(int i) {
	if (!sufficientStatisticsKnown) {
		computeRemainingStatistics(true, i);
	}
	return hBeta[i];
}

bool CyclicCoordinateDescent::getFixedBeta(int i) {
	return fixBeta[i];
}

void CyclicCoordinateDescent::setFixedBeta(int i, bool value) {
	fixBeta[i] = value;
}

//void CyclicCoordinateDescent::setZeroBetaFixed(void) {
//	for (int j = 0; j < J; ++j) {
//		if (hBeta[j] == static_cast<real>(0.0)) {
//			setFixedBeta(j, true);
//		}
//	}
//}

void CyclicCoordinateDescent::checkAllLazyFlags(void) {
	if (!xBetaKnown) {
		computeXBeta();
	}

	if (!validWeights) {
		computeNEvents();
	}

	if (!sufficientStatisticsKnown) {
		computeRemainingStatistics(true, 0); // TODO Check index?
	}
}

double CyclicCoordinateDescent::getLogLikelihood(void) {

	checkAllLazyFlags();

	getDenominators();
	likelihoodCount += 1;

	return modelSpecifics.getLogLikelihood(useCrossValidation);
}

void CyclicCoordinateDescent::getDenominators() {
	// Do nothing
}

double convertVarianceToHyperparameter(double value) {
	return sqrt(2.0 / value);
}

double convertHyperparameterToVariance(double value) {
	return 2.0 / (value * value);
}

void CyclicCoordinateDescent::setHyperprior(double value) {
	sigma2Beta = value;
	lambda = convertVarianceToHyperparameter(value);
}

double CyclicCoordinateDescent::getHyperprior(void) const {
	double hyperPrior;
	switch (priorType) {
		case NONE : hyperPrior = 0.0; break;
		case LAPLACE : hyperPrior = convertHyperparameterToVariance(lambda); break;
		case NORMAL : hyperPrior = sigma2Beta; break;
		default : hyperPrior = 0.0; break;
	}
	return hyperPrior;
}

void CyclicCoordinateDescent::makeDirty(void) {
	xBetaKnown = false;
	validWeights = false;
	sufficientStatisticsKnown = false;
}

void CyclicCoordinateDescent::setPriorType(int iPriorType) {
	if (iPriorType < NONE || iPriorType > NORMAL) {
		cerr << "Unknown prior type" << endl;
		exit(-1);
	}
	priorType = iPriorType;
}

void CyclicCoordinateDescent::setBeta(const std::vector<double>& beta) {
	for (int j = 0; j < J; ++j) {
		hBeta[j] = static_cast<real>(beta[j]);
	}
	xBetaKnown = false;
	sufficientStatisticsKnown = false;
	fisherInformationKnown = false;
}

void CyclicCoordinateDescent::setBeta(int i, double beta) {
//	sufficientStatisticsKnown = false;
//	double delta = beta - hBeta[i];
//	updateSufficientStatistics(delta, i);

	hBeta[i] = static_cast<real>(beta);
	xBetaKnown = false;
	sufficientStatisticsKnown = false;
	fisherInformationKnown = false;
}

void CyclicCoordinateDescent::setWeights(real* iWeights) {

	if (iWeights == NULL) {
		if (hWeights) {
			free(hWeights);
			hWeights = NULL;
		}
		std::cerr << "Turning off weights!" << std::endl;
		// Turn off weights
		useCrossValidation = false;
		validWeights = false;
		sufficientStatisticsKnown = false;
	} else {

		if (hWeights == NULL) {
			hWeights = (real*) malloc(sizeof(real) * K);
		}
		for (int i = 0; i < K; ++i) {
			hWeights[i] = iWeights[i];
		}
		useCrossValidation = true;
		validWeights = false;
		sufficientStatisticsKnown = false;
	}
}
	
double CyclicCoordinateDescent::getLogPrior(void) {
	double value;
	if (priorType == NONE) {
		value = 0.0;
	}
	else if (priorType == LAPLACE) {
		value = J * log(0.5 * lambda) - lambda * oneNorm(hBeta, J);
	} else {
		value = -0.5 * J * log(2.0 * PI * sigma2Beta) - 0.5 * twoNormSquared(hBeta, J) / sigma2Beta;
	}
	// TODO INTERCEPT
// 	if (priorType == LAPLACE) {
// 		value -= log(0.5 * lambda) - lambda * std::abs(hBeta[0]);
// 	} else {
// 		value -= -0.5 * log(2.0 * PI * sigma2Beta) - 0.5 * (hBeta[0] * hBeta[0]) / sigma2Beta;
// 	}

	//return value;
	return 0;
}

double CyclicCoordinateDescent::getObjectiveFunction(int convergenceType) {
	if (convergenceType == GRADIENT) {
		real criterion = 0;
		if (useCrossValidation) {
			for (int i = 0; i < K; i++) {
				criterion += hXBeta[i] * hY[i] * hWeights[i];
			}
		} else {
			for (int i = 0; i < K; i++) {
				criterion += hXBeta[i] * hY[i];
			}
		}
		return static_cast<double> (criterion);
	}
	if (convergenceType == MITTAL) {
		return getLogLikelihood();
	}
	if (convergenceType == LANGE) {
		return getLogLikelihood() + getLogPrior();
	}
	cerr << "Invalid convergence type: " << convergenceType << endl;
	exit(-1);
}

double CyclicCoordinateDescent::computeZhangOlesConvergenceCriterion(void) {
	double sumAbsDiffs = 0;
	double sumAbsResiduals = 0;
	if (useCrossValidation) {
		for (int i = 0; i < K; i++) {
			sumAbsDiffs += abs(hXBeta[i] - hXBetaSave[i]) * hWeights[i];
			sumAbsResiduals += abs(hXBeta[i]) * hWeights[i];
		}
	} else {
		for (int i = 0; i < K; i++) {
			sumAbsDiffs += abs(hXBeta[i] - hXBetaSave[i]);
			sumAbsResiduals += abs(hXBeta[i]);
		}
	}
	return sumAbsDiffs / (1.0 + sumAbsResiduals);
}

void CyclicCoordinateDescent::saveXBeta(void) {
	memcpy(hXBetaSave, hXBeta, K * sizeof(real));
}

void CyclicCoordinateDescent::update(
		int maxIterations,
		int convergenceType,
		double epsilon
		) {

	if (convergenceType < GRADIENT || convergenceType > ZHANG_OLES) {
		cerr << "Unknown convergence criterion: " << convergenceType << endl;
		exit(-1);
	}

	if (!validWeights) {
		computeNEvents();
		computeFixedTermsInLogLikelihood();
		computeFixedTermsInGradientAndHessian();
		validWeights = true;
	}

	if (!xBetaKnown) {
		computeXBeta();
		xBetaKnown = true;
		sufficientStatisticsKnown = false;
	}

	if (!sufficientStatisticsKnown) {
		computeRemainingStatistics(true, 0); // TODO Check index?
		sufficientStatisticsKnown = true;
	}

	resetBounds();

	bool done = false;
	int iteration = 0;
	double lastObjFunc;

	if (convergenceType < ZHANG_OLES) {
		lastObjFunc = getObjectiveFunction(convergenceType);
	} else { // ZHANG_OLES
		saveXBeta();
	}
	
	while (!done) {
	
		// Do a complete cycle
		for(int index = 0; index < J; index++) {
		
			if (!fixBeta[index]) {
				double delta = ccdUpdateBeta(index);
				delta = applyBounds(delta, index);
				if (delta != 0.0) {
					sufficientStatisticsKnown = false;
					updateSufficientStatistics(delta, index);
					computeRemainingStatistics(true, 0); // TODO REMOVE, only for CLR
				}
			}
			
			if ( (noiseLevel > QUIET) && ((index+1) % 100 == 0)) {
				cout << "Finished variable " << (index+1) << endl;
			}
			
		}

		iteration++;
//		bool checkConvergence = (iteration % J == 0 || iteration == maxIterations);
		bool checkConvergence = true; // Check after each complete cycle

		if (checkConvergence) {

			double conv;
			bool illconditioned = false;
			if (convergenceType < ZHANG_OLES) {
 				double thisObjFunc = getObjectiveFunction(convergenceType);
				if (thisObjFunc != thisObjFunc) {
					cout << endl << "Warning! problem is ill-conditioned for this choice of hyperparameter. Enforcing convergence!" << endl;
					conv = 0.0;
					illconditioned = true;
				} else {
					conv = computeConvergenceCriterion(thisObjFunc, lastObjFunc);
				}
				lastObjFunc = thisObjFunc;
			} else { // ZHANG_OLES
				conv = computeZhangOlesConvergenceCriterion();
				saveXBeta();
			} // Necessary to call getObjFxn or computeZO before getLogLikelihood,
			  // since these copy over XBeta

			double thisLogLikelihood = getLogLikelihood();
			double thisLogPrior = getLogPrior();
			double thisLogPost = thisLogLikelihood + thisLogPrior;

			if (noiseLevel > QUIET) {
				cout << endl;
				printVector(hBeta, J, cout);
				cout << endl;
				cout << "log post: " << thisLogPost
						<< " (" << thisLogLikelihood << " + " << thisLogPrior
						<< ") (iter:" << iteration << ") ";
			}

			if (epsilon > 0 && conv < epsilon) {
				if (illconditioned) {
					lastReturnFlag = ILLCONDITIONED;
				} else {
					cout << "Reached convergence criterion" << endl;
					lastReturnFlag = SUCCESS;
				}
				done = true;
			} else if (iteration == maxIterations) {
				cout << "Reached maximum iterations" << endl;
				done = true;
				lastReturnFlag = MAX_ITERATIONS;
			} else {
				if (noiseLevel > QUIET) {
					cout << endl;
				}
			}
		}				
	}
	lastIterationCount = iteration;
	updateCount += 1;

	fisherInformationKnown = false;
}

/**
 * Computationally heavy functions
 */

void CyclicCoordinateDescent::computeGradientAndHessian(int index, double *ogradient,
		double *ohessian) {
	modelSpecifics.computeGradientAndHessian(index, ogradient, ohessian, useCrossValidation);
}

void CyclicCoordinateDescent::computeNumeratorForGradient(int index) {
	// Delegate
	modelSpecifics.computeNumeratorForGradient(index);
}

void CyclicCoordinateDescent::computeRatiosForGradientAndHessian(int index) {
	cerr << "Error!" << endl;
	exit(-1);
}

double CyclicCoordinateDescent::getHessianDiagonal(int index) {

	checkAllLazyFlags();
	double g_d1, g_d2;

	computeNumeratorForGradient(index);
	computeGradientAndHessian(index, &g_d1, &g_d2);

	return g_d2;
}

double CyclicCoordinateDescent::getAsymptoticVariance(int indexOne, int indexTwo) {
	checkAllLazyFlags();
	if (!fisherInformationKnown) {
		computeAsymptoticVarianceMatrix();
		fisherInformationKnown = true;
	}

	IndexMap::iterator itOne = hessianIndexMap.find(indexOne);
	IndexMap::iterator itTwo = hessianIndexMap.find(indexTwo);

	if (itOne == hessianIndexMap.end() || itTwo == hessianIndexMap.end()) {
		return NAN;
	} else {
		return hessianMatrix(itOne->second, itTwo->second);
	}
}

void CyclicCoordinateDescent::computeAsymptoticVarianceMatrix(void) {

	typedef std::vector<int> int_vec;
	int_vec indices;
	hessianIndexMap.clear();

	int index = 0;
	for (int j = 0; j < J; ++j) {
		if (getBeta(j) != 0.0) {
			indices.push_back(j);
			hessianIndexMap[j] = index;
			index++;
		}
	}

	hessianMatrix.resize(indices.size(), indices.size());
	modelSpecifics.makeDirty(); // clear hessian terms

	for (int ii = 0; ii < indices.size(); ++ii) {
		for (int jj = ii; jj < indices.size(); ++jj) {
			const int i = indices[ii];
			const int j = indices[jj];
//			std::cerr << "(" << i << "," << j << ")" << std::endl;
			double fisherInformation = 0.0;
			modelSpecifics.computeFisherInformation(i, j, &fisherInformation, useCrossValidation);
//			if (fisherInformation != 0.0) {
				// Add tuple to sparse matrix
//				tripletList.push_back(Triplet<double>(ii,jj,fisherInformation));
//			}
			hessianMatrix(jj,ii) = hessianMatrix(ii,jj) = fisherInformation;

//			std::cerr << "Info = " << fisherInformation << std::endl;
//			std::cerr << "Hess = " << getHessianDiagonal(0) << std::endl;
//			exit(-1);
		}
	}

//	sm.setFromTriplets(tripletList.begin(), tripletList.end());
//	cout << sm;
//	auto inv = sm.triangularView<Upper>().solve(dv1);
//	cout << sm.inverse();
//	cout << hessianMatrix << endl;

	// Take inverse
	hessianMatrix = hessianMatrix.inverse();

//	cout << hessianMatrix << endl;
}

double CyclicCoordinateDescent::ccdUpdateBeta(int index) {

	double delta;

	if (!sufficientStatisticsKnown) {
		cerr << "Error in state synchronization." << endl;
		exit(0);		
	}
	
	computeNumeratorForGradient(index);
	
	double g_d1;
	double g_d2;
					
	computeGradientAndHessian(index, &g_d1, &g_d2);

//	if (g_d2 <= 0.0) {
//		cerr << "Not positive-definite! Hessian = " << g_d2 << endl;
//		exit(-1);
//	}

	// Move into separate delegate-function (below)

//	if (index > 0) { // TODO Bad hard coding, INTERCEPT

	if (priorType == NORMAL) {

#if 1
		delta = - (g_d1 + (hBeta[index] / sigma2Beta)) /
				  (g_d2 + (1.0 / sigma2Beta));

#else			  
		delta = - (g_d1 + 2*hBeta[index]*sigma2Beta) / (g_d2 + 2*sigma2Beta);
#endif
				  
		
	} else if (priorType == LAPLACE){
					
		double neg_update = - (g_d1 - 0.0) / g_d2;
		double pos_update = - (g_d1 + 0.0) / g_d2;

		//cout << "step = " << g_d1/g_d2 << endl;

		int signBetaIndex = sign(hBeta[index]);
		
		if (signBetaIndex == 0) {
						
			if (neg_update < 0.0) {
				delta = neg_update;
			} else if (pos_update > 0.0) {
				delta = pos_update;
			} else {
				delta = 0.0;
			}
		} else { // non-zero entry
			
			if (signBetaIndex < 0) {
				delta = neg_update;
			} else {
				delta = pos_update;			
			}
			
			if ( sign(hBeta[index] + delta) != signBetaIndex ) {
				delta = - hBeta[index];
			}			
		}
	} else {
		delta = -g_d1 / g_d2; // No regularization
	}

//	} else { // TODO INTERCEPT
//		delta = -g_d1 / g_d2;
//	}
	
	return delta;
}

template <class IteratorType>
void CyclicCoordinateDescent::axpy(real* y, const real alpha, const int index) {
	IteratorType it(*hXI, index);
	for (; it; ++it) {
		const int k = it.index();
		y[k] += alpha * it.value();
	}
}

void CyclicCoordinateDescent::computeXBeta(void) {
	// Note: X is current stored in (sparse) column-major format, which is
	// inefficient for forming X\beta.
	// TODO Make row-major version of X

	// clear X\beta
	zeroVector(hXBeta, K);

	// Update one column at a time (poor cache locality)
	for (int j = 0; j < J; ++j) {
		const real beta = hBeta[j];
		if (beta != static_cast<real>(0)) {
			switch (hXI->getFormatType(j)) {
			case INDICATOR:
				axpy<IndicatorIterator>(hXBeta, beta, j);
				break;
			case DENSE:
				axpy<DenseIterator>(hXBeta, beta, j);
				break;
			case SPARSE:
				axpy<SparseIterator>(hXBeta, beta, j);
				break;
			default:
				// throw error
				exit(-1);
			}
		}
	}
}

void CyclicCoordinateDescent::updateXBeta(double delta, int index) {
	// Update beta
	real realDelta = static_cast<real>(delta);

	hBeta[index] += realDelta;

	// Delegate
	modelSpecifics.updateXBeta(realDelta, index, useCrossValidation);
}

void CyclicCoordinateDescent::updateSufficientStatistics(double delta, int index) {
	updateXBeta(delta, index);
	sufficientStatisticsKnown = true;
}

void CyclicCoordinateDescent::computeRemainingStatistics(bool allStats, int index) { // TODO Rename
	// Separate function for benchmarking
	if (allStats) {
		// Delegate
		modelSpecifics.computeRemainingStatistics(useCrossValidation);
	}
}

double CyclicCoordinateDescent::oneNorm(real* vector, const int length) {
	double norm = 0;
	for (int i = 0; i < length; i++) {
		norm += abs(vector[i]);
	}
	return norm;
}

double CyclicCoordinateDescent::twoNormSquared(real * vector, const int length) {
	double norm = 0;
	for (int i = 0; i < length; i++) {
		norm += vector[i] * vector[i];
	}
	return norm;
}

/**
 * Updating and convergence functions
 */

double CyclicCoordinateDescent::computeConvergenceCriterion(double newObjFxn, double oldObjFxn) {	
	// This is the stopping criterion that Ken Lange generally uses
	return abs(newObjFxn - oldObjFxn) / (abs(newObjFxn) + 1.0);
}

double CyclicCoordinateDescent::applyBounds(double inDelta, int index) {
	double delta = inDelta;
	if (delta < -hDelta[index]) {
		delta = -hDelta[index];
	} else if (delta > hDelta[index]) {
		delta = hDelta[index];
	}

	hDelta[index] = max(2.0 * abs(delta), 0.5 * hDelta[index]);
	return delta;
}

/**
 * Utility functions
 */

void CyclicCoordinateDescent::computeFixedTermsInLogLikelihood(void) {
	modelSpecifics.computeFixedTermsInLogLikelihood(useCrossValidation);
}

void CyclicCoordinateDescent::computeFixedTermsInGradientAndHessian(void) {
	// Delegate
	modelSpecifics.computeFixedTermsInGradientAndHessian(useCrossValidation);
}



template <class T>
void CyclicCoordinateDescent::printVector(T* vector, int length, ostream &os) {
	os << "(" << vector[0];
	for (int i = 1; i < length; i++) {
		os << ", " << vector[i];
	}
	os << ")";			
}

template <class T> 
T* CyclicCoordinateDescent::readVector(
		const char *fileName,
		int *length) {

	ifstream fin(fileName);
	T d;
	std::vector<T> v;
	
	while (fin >> d) {
		v.push_back(d);
	}
	
	T* ptr = (T*) malloc(sizeof(T) * v.size());
	for (int i = 0; i < v.size(); i++) {
		ptr[i] = v[i];
	}
		
	*length = v.size();
	return ptr;
}

void CyclicCoordinateDescent::testDimension(int givenValue, int trueValue, const char *parameterName) {
	if (givenValue != trueValue) {
		cerr << "Wrong dimension in " << parameterName << " vector." << endl;
		exit(-1);
	}
}	

inline int CyclicCoordinateDescent::sign(double x) {
	if (x == 0) {
		return 0;
	}
	if (x < 0) {
		return -1;
	}
	return 1;
}

} // namespace
