//
//  Copyright (c) 2015 Nikolay Volosatov. All rights reserved.
//

#include "field.h"
#include "factors.h"
#include <iostream>
#include <cmath>
#include <mpi.h>

//#define DEBUG_PRINT

static int const MASTER = 0;
static size_t const MAX_ITTERATIONS_COUNT = 50;
static int const NOBODY = -1;
static int const NOTHING = -1;

static int const MAX_ROW_INDEX = 100000;
static int const TAG_TOP_TO_BOTTOM_ROWS = 23;
static int const TAG_BOTTOM_TO_TOP_ROWS = 24;
static int const TAG_LEFT_TO_RIGHT_ROW = 1 * MAX_ROW_INDEX;
static int const TAG_RIGHT_TO_LEFT_ROW = 2 * MAX_ROW_INDEX;
static int const TAG_FIRST_PASS = 3 * MAX_ROW_INDEX;
static int const TAG_SECOND_PASS = 4 * MAX_ROW_INDEX;

Field::Field() {
    width = ftr.X1SplitCount();
    height = ftr.X2SplitCount();

    hX = ftr.X1() / (width - 1);
    hY = ftr.X2() / (height - 1);
    dT = ftr.totalTime() / ftr.TimeSplitCount();
    epsilon = ftr.Epsilon();
    transposed = false;
    fout = NULL;
    mfout = NULL;

    calculateNBS();

    prev = new double[height * width];
    curr = new double[height * width];
    buff = new double[height * width];
    views = new double[ftr.ViewCount()];

    size_t maxDim = std::max(width, height);
    aF = new double[maxDim];
    bF = new double[maxDim];
    cF = new double[maxDim];
    fF = new double[maxDim];

    if (ftr.EnablePlot()) {
        enablePlotOutput();
    }
    if (ftr.EnableMatrix()) {
        enableMatrixOutput();
    }
}

Field::~Field() {
    if (fout != NULL) {
        fout->close();
        delete fout;
    }

    if (mfout != NULL) {
        mfout->close();
        delete mfout;
    }
    
    delete[] prev;
    delete[] curr;
    delete[] buff;
    delete[] views;

    delete[] aF;
    delete[] bF;
    delete[] cF;
    delete[] fF;
}

void Field::calculateNBS() {
    int numProcs;
    MPI_Comm_size(MPI_COMM_WORLD, &numProcs);
    MPI_Comm_rank(MPI_COMM_WORLD, &myId);

    mySX = 0; mySY = 0;
    leftN = NOBODY; rightN = NOBODY; topN = NOBODY; bottomN = NOBODY;

    int stX = 1, stY = numProcs;
    calculateGrid(numProcs, (double)width / height, stX, stY);

    int i = myId / stX;
    int j = myId % stX;

    size_t newHeight = height / stY;
    mySY = newHeight * i;
    if (i == stY - 1) {
        newHeight = height - newHeight * (stY - 1);
    }
    newHeight += (i > 0 && i < stY - 1) ? 2 : (stY == 1 ? 0 : 1);

    size_t newWidth = width / stX;
    mySX = newWidth * j;
    if (j == stX - 1) {
        newWidth = width - newWidth * (stX - 1);
    }
    newWidth += (i > 0 && i < stX - 1) ? 2 : (stX == 1 ? 0 : 1);

    if (i > 0) {
        topN = myId - stX;
    }
    if (i < stY - 1) {
        bottomN = myId + stX;
    }

    if (j > 0) {
        leftN = myId - 1;
    }
    if (j < stX - 1) {
        rightN = myId + 1;
    }

    width = newWidth;
    height = newHeight;

    //printf("I'm %d\twith w:%zu\th:%zu.\tTop:%d\tbottom:%d\tleft:%d\tright:%d\n", myId, width, height, topN, bottomN, leftN, rightN);
}

void Field::calculateGrid(int numProcs, double stExpected, int &stX, int &stY) {
    stX = 1; stY = numProcs;

    bool withSwap = false;
    if (stExpected > 1) {
        stExpected = 1.0 / stExpected;
        withSwap = true;
    }

    double stFactor = 1.0 / numProcs;
    for (int k = 2, len = numProcs / 2; k < len; ++k) {
        int y = numProcs / k;
        double newStFactor = (double)k / y;
        if (numProcs % k == 0 && fabs(newStFactor - stExpected) < fabs(stFactor - stExpected)) {
            stX = k; stY = y;
            stFactor = newStFactor;
        }
    }

    if (withSwap) {
        std::swap(stX, stY);
    }
}

void Field::randomFill() {
    for (size_t index = 0, len = width * height; index < len; ++index) {
        curr[index] = rand() % 100;
    }
}

double Field::view(double x1, double x2) {
    ssize_t x1index = floor(x1 / hX) - mySX + (leftN != NOBODY ? 1 : 0);
    ssize_t x2index = floor(x2 / hY) - mySY + (topN != NOBODY ? 1 : 0);

    bool inMyX1 = x1index < (leftN != NOBODY ? 1 : 0) || x1index >= width - (rightN != NOBODY ? 1 : 0);
    bool inMyX2 = x2index < (topN != NOBODY ? 1 : 0) || x2index >= height - (bottomN != NOBODY ? 1 : 0);
    if (inMyX1 == false || inMyX2 == false) {
        return NOTHING;
    }

    /*double x1factor = x1 - x1index * hX;
    double x2factor = x2 - x2index * hY;

    double value = curr[x2index * width + x1index] + x1factor * curr[x2index * width + x1index + 1];
    value += x2factor * (curr[(x2index + 1) * width + x1index] + x1factor * curr[(x2index + 1) * width + x1index + 1]);*/

    return curr[x2index * width + x1index];
}

double Field::view(size_t index) {
    return view(ftr.X1View(index), ftr.X2View(index));
}

void Field::enablePlotOutput() {
    if (myId != MASTER) {
        return;
    }

    if (fout != NULL) {
        fout->close();
    }
    fout = new std::ofstream("view.csv");
}

void Field::enableMatrixOutput() {
    if (myId != MASTER) {
        return;
    }

    if (mfout != NULL) {
        mfout->close();
    }
    mfout = new std::ofstream("matrix.csv");
}

void Field::printMatrix() {
    // TODO: print without overlapses
    if (mfout != NULL && t > nextFrameTime) {
        nextFrameTime += ftr.totalTime() / ftr.MatrixFramesCount();

        size_t index = 0;
        for (size_t row = 0; row < height; ++row) {
            for (size_t col = 0; col < width; ++col, ++index) {
                *mfout << curr[index];
                if (col < width - 1) {
                    *mfout << " ";
                }
            }
            *mfout << "\n";
        }
        *mfout << "\n";
        mfout->flush();
    }
}

void Field::printViews() {
    if (fout != NULL) {
        reduceViews();

        *fout << t;
        for (size_t index = 0, len = ftr.ViewCount(); index < len; ++index) {
            *fout << "," << views[index];
        }
        *fout << "\n";
        fout->flush();
    }
}

void Field::print() {
    double viewValue = view(ftr.DebugView());

    if (fabs(viewValue - NOTHING) > __DBL_EPSILON__) {
        printf("Field (itrs: %zu, time: %.5f)\tview: %.7f\n",
               lastIterrationsCount, t, viewValue);
    }
}

void Field::debug(const char *name) {
#ifdef DEBUG_PRINT
    printf("I'm %d before %s\n", myId, name);
#endif
}

void Field::fillInitial() {
    t = 0;
    lastIterrationsCount = 0;
    if (transposed) {
        transpose();
    }

    for (size_t index = 0, len = width * height; index < len; ++index) {
        curr[index] = ftr.TStart();
    }
}

void Field::transpose(double *arr) {
    for (size_t index = 0, len = width * height; index < len; ++index) {
        size_t newIndex = (index % width) * height + index / width;
        buff[newIndex] = arr[index];
    }
    for (size_t index = 0, len = width * height; index < len; ++index) {
        arr[index] = buff[index];
    }
}

void Field::transpose() {
    sendReceivePrevRows();
    transpose(prev);
    transpose(curr);
    
    std::swap(width, height);
    std::swap(hX, hY);
    transposed = transposed == false;

    std::swap(leftN, topN);
    std::swap(rightN, bottomN);
}

void Field::nextTimeLayer() {
    std::swap(curr, prev);
    t += dT;
}

void Field::fillFactors(size_t row, bool first) {
    double *rw = prev + row * width;
    double *brw = first && transposed == false ? rw : (curr + row * width);

    if (leftN == NOBODY) {
        double lm0 = ftr.lambda(brw[0]), lmh = ftr.lambda(brw[1]);
        aF[0] = 0;
        cF[0] = dT * (lm0 + lmh) + hX * hX * ftr.ro(brw[0]) * ftr.cEf(brw[0]);
        bF[0] = -dT * (lm0 + lmh);
        fF[0] = hX * hX * ftr.ro(brw[0]) * ftr.cEf(brw[0]) * rw[0];
    }

    if (rightN == NOBODY) {
        double TPrev = brw[width - 1];
        double TPrev4 = TPrev * TPrev * TPrev * TPrev;
        double lmXX = ftr.lambda(brw[width - 1]), lmXXm1 = ftr.lambda(brw[width - 2]);
        aF[width - 1] = -dT * (lmXXm1 + lmXX);
        cF[width - 1] = dT * (lmXXm1 + lmXX)
                         + hX * hX * ftr.ro(brw[width - 1]) * ftr.cEf(brw[width - 1])
                         + 2 * hX * dT * ftr.alpha(t);
        bF[width - 1] = 0;
        fF[width - 1] = hX * hX * ftr.ro(brw[width - 1]) * ftr.cEf(brw[width - 1]) * rw[width - 1]
                         - 2 * hX * dT * ftr.sigma(t) * (TPrev4 - ftr.TEnv4())
                         + 2 * hX * dT * ftr.alpha(t) * ftr.TEnv();
    }

    for (size_t index = 1; index < width - 1; ++index) {
        double roc = ftr.ro(brw[index]) * ftr.cEf(brw[index]);
        double lmXm1 = ftr.lambda(brw[index - 1]), lmX = ftr.lambda(brw[index]), lmXp1 = ftr.lambda(brw[index + 1]);

        aF[index] = dT * (lmX + lmXm1);
        bF[index] = dT * (lmXp1 + lmX);
        cF[index] = -dT * (lmXp1 + 2 * lmX + lmXm1) - 2 * hX * hX * roc;
        fF[index] = -2 * hX * hX * roc * rw[index];
    }
}

double Field::solve(size_t row, bool first) {
    receiveFirstPass(row);
    double m = 0;
    for (size_t i = 1; i < width; ++i) {
        m = aF[i] / cF[i - 1];
        cF[i] -= m * bF[i - 1];
        fF[i] -= m * fF[i - 1];
    }
    sendFirstPass(row);

    double *y = curr + row * width;
    double *py = first ? (prev + row * width) : y;

    double newValue = 0, maxDelta = 0;
    if (rightN == NOBODY) {
        newValue = fF[width - 1] / cF[width - 1];
        maxDelta = fabs(newValue - py[width - 1]);
        y[width - 1] = newValue;
    } else {
        receiveSecondPass(row);
    }

    for (ssize_t i = width - 2; i >= 0; --i) {
        newValue = (fF[i] - bF[i] * y[i + 1]) / cF[i];

        double newDelta = fabs(newValue - py[i]);
        maxDelta = std::max(maxDelta, newDelta);
        y[i] = newValue;
    }
    sendSecondPass(row);

    reduceMaxDelta(maxDelta);
    sendReceiveCurrRowBorders(row);

    return maxDelta;
}

void Field::test() {
    size_t w = width;
    aF[0] = 0; aF[1] = 1; aF[2] = 1; aF[3] = 1; aF[4] = 1;
    bF[0] = 3; bF[1] = -1; bF[2] = 1; bF[3] = -1; bF[4] = 0;
    cF[0] = 4; cF[1] = 2; cF[2] = 4; cF[3] = 2; cF[4] = 2;
    fF[0] = 4; fF[1] = 2; fF[2] = 7.5; fF[3] = 1; fF[4] = -3;
    prev[0] = -0.540909; prev[1] = 2.05455; prev[2] = 1.56818; prev[3] = -0.827273; prev[4] = -1.08636;

    width = 5;

    solve(0, true);

    for (size_t i = 0; i < 5; ++i) {
        if (fabs(prev[i] - curr[i]) > 0.00001) {
            printf("Error: %lu\n", i);
        }
    }

    width = w;
}

size_t Field::solveRows() {
    size_t maxIterationsCount = 0;

    for (size_t row = (topN != NOBODY ? 1 : 0); row < height - (bottomN != NOBODY ? 1 : 0); ++row) {
        debug("fillFactors");
        fillFactors(row, true);
        debug("solve first");
        double delta = solve(row, true);
        size_t iterationsCount = 1;

        while (delta > epsilon) {
            debug("solve than");
            fillFactors(row, false);
            delta = solve(row, false);
            ++iterationsCount;

            /*if (iterationsCount > MAX_ITTERATIONS_COUNT / 2) {
                printf("Warning! Iterations (%lu:%lu) on layer going to maximum\n",
                       transposed ? 0 : row, transposed ? row : 0);
            }*/

            if (iterationsCount > MAX_ITTERATIONS_COUNT) {
                /*printf("Error! Iterations (%lu:%lu) on layer [t: %.3f, index: %lu, delta: %.5f] achieve maximum\n",
                       transposed ? 0 : row, transposed ? row : 0, t, (unsigned long)(t / dT), delta);*/
                //exit(1);
                break;
            }
        }
        maxIterationsCount = std::max(maxIterationsCount, iterationsCount);
    }

    return maxIterationsCount;
}

void Field::solve() {
    if (done()) {
        return;
    }

    lastIterrationsCount = 0;

    debug("nextTimeLayer h");
    nextTimeLayer();
    lastIterrationsCount += solveRows();

    debug("nextTimeLayer v");
    nextTimeLayer();
    transpose();
    lastIterrationsCount += solveRows();
    transpose();

    printMatrix();
    printViews();
    print();
}

double Field::time() {
    return t;
}

bool Field::done() {
    return t >= ftr.totalTime();
}

void Field::sendReceivePrevRows() {
    if (topN != NOBODY && bottomN != NOBODY) {
        MPI_Sendrecv(prev + (height - 2) * width, (int)width, MPI_DOUBLE, bottomN, TAG_TOP_TO_BOTTOM_ROWS,
                     prev, (int)width, MPI_DOUBLE, topN, TAG_TOP_TO_BOTTOM_ROWS,
                     MPI_COMM_WORLD, MPI_STATUS_IGNORE);
        MPI_Sendrecv(prev + width, (int)width, MPI_DOUBLE, topN, TAG_BOTTOM_TO_TOP_ROWS,
                     prev + (height - 1) * width, (int)width, MPI_DOUBLE, bottomN, TAG_BOTTOM_TO_TOP_ROWS,
                     MPI_COMM_WORLD, MPI_STATUS_IGNORE);
    }
    else if (topN != NOBODY) {
        MPI_Recv(prev, (int)width, MPI_DOUBLE, topN, TAG_TOP_TO_BOTTOM_ROWS, MPI_COMM_WORLD, MPI_STATUS_IGNORE);
        MPI_Send(prev + width, (int)width, MPI_DOUBLE, topN, TAG_BOTTOM_TO_TOP_ROWS, MPI_COMM_WORLD);
    }
    else if (bottomN != NOBODY) {
        MPI_Send(prev + (height - 2) * width, (int)width, MPI_DOUBLE, bottomN, TAG_TOP_TO_BOTTOM_ROWS, MPI_COMM_WORLD);
        MPI_Recv(prev + (height - 1) * width, (int)width, MPI_DOUBLE, bottomN, TAG_BOTTOM_TO_TOP_ROWS, MPI_COMM_WORLD, MPI_STATUS_IGNORE);
    }
}

void Field::sendReceiveCurrRowBorders(size_t row) {
    double *rw = curr + row * width;

    if (leftN != NOBODY && rightN != NOBODY) {
        MPI_Sendrecv(rw + (width - 2), 1, MPI_DOUBLE, rightN, TAG_LEFT_TO_RIGHT_ROW + (int)row,
                     rw, 1, MPI_DOUBLE, leftN, TAG_LEFT_TO_RIGHT_ROW + (int)row,
                     MPI_COMM_WORLD, MPI_STATUS_IGNORE);
        MPI_Sendrecv(rw + 1, 1, MPI_DOUBLE, leftN, TAG_RIGHT_TO_LEFT_ROW + (int)row,
                     rw + (width - 1), 1, MPI_DOUBLE, rightN, TAG_RIGHT_TO_LEFT_ROW + (int)row,
                     MPI_COMM_WORLD, MPI_STATUS_IGNORE);
    }
    else if (leftN != NOBODY) {
        MPI_Recv(rw, 1, MPI_DOUBLE, leftN, TAG_LEFT_TO_RIGHT_ROW + (int)row, MPI_COMM_WORLD, MPI_STATUS_IGNORE);
        MPI_Send(rw + 1, 1, MPI_DOUBLE, leftN, TAG_RIGHT_TO_LEFT_ROW + (int)row, MPI_COMM_WORLD);
    }
    else if (rightN != NOBODY) {
        MPI_Send(rw + (width - 2), 1, MPI_DOUBLE, rightN, TAG_LEFT_TO_RIGHT_ROW + (int)row, MPI_COMM_WORLD);
        MPI_Recv(rw + (width - 1), 1, MPI_DOUBLE, rightN, TAG_RIGHT_TO_LEFT_ROW + (int)row, MPI_COMM_WORLD, MPI_STATUS_IGNORE);
    }
}

void Field::sendFirstPass(size_t row) {
    if (rightN != NOBODY) {
        double bff[] = { bF[width - 2], cF[width - 2], fF[width - 2] };
        MPI_Send(bff, 3, MPI_DOUBLE, rightN, TAG_FIRST_PASS + (int)row, MPI_COMM_WORLD);
    }
}

void Field::receiveFirstPass(size_t row) {
    if (leftN != NOBODY) {
        double bff[3];
        MPI_Recv(bff, 3, MPI_DOUBLE, leftN, TAG_FIRST_PASS + (int)row, MPI_COMM_WORLD, MPI_STATUS_IGNORE);
        bF[0] = bff[0];
        cF[0] = bff[1];
        fF[0] = bff[2];
    }
}

void Field::sendSecondPass(size_t row) {
    double *y = curr + row * width;
    if (leftN != NOBODY) {
        MPI_Send(y + 1, 1, MPI_DOUBLE, leftN, TAG_SECOND_PASS + (int)row, MPI_COMM_WORLD);
    }
}

void Field::receiveSecondPass(size_t row) {
    double *y = curr + row * width;
    if (rightN != NOBODY) {
        MPI_Recv(y + width - 1, 1, MPI_DOUBLE, rightN, TAG_SECOND_PASS + (int)row, MPI_COMM_WORLD, MPI_STATUS_IGNORE);
    }
}

void Field::reduceMaxDelta(double &maxDelta) {
    double result = 0; // TODO: Check if it could be the same pointer with maxDelta
    MPI_Allreduce(&maxDelta, &result, 1, MPI_DOUBLE, MPI_MAX, MPI_COMM_WORLD);
    maxDelta = result;
}

void Field::reduceViews() {
    int viewsCount = (int)ftr.ViewCount();
    double *tmpViews = new double[viewsCount];
    for (size_t index = 0; index < viewsCount; ++index) {
        tmpViews[index] = view(index);
    }

    MPI_Reduce(tmpViews, views, viewsCount, MPI_DOUBLE, MPI_MAX, MASTER, MPI_COMM_WORLD);
}
