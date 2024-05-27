/*
Copyright (c) 2024 Quantrosoft Pte. Ltd.

MIT License
Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL
THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
*/


#include <filesystem>
#include <string>
#include <iostream>
#include <fstream>
#include <sstream>
#include <vector>
#include <string>
#include <iomanip>
#include <ctime>
#include <limits>
#include <zlib.h>
#include <ATLComTime.h>

#include <zorro.h>


#pragma region History
std::string Version = "cTraderCache V1.0";
// V1.0     20.05.24    HMz created
#pragma endregion


#pragma region Structs, classes, constants
struct GLOBAL
{
   int Diag;
   int HttpId;
   int PriceType, VolType, OrderType;
   double Unit;
   char Url[256]; // send URL buffer
   char Key[256], Secret[256]; // credentials
   char Symbol[64], Uuid[256]; // last trade, symbol and Uuid
   char AccountId[16]; // account currency
} G; // parameter singleton

struct SerialArrays
{
public:
   std::vector<uint64_t> Tick2dt;
   std::vector<uint64_t> Tick2Bid;
   std::vector<uint64_t> Tick2Ask;
};
#pragma endregion


#pragma region Member variables
bool mIs1stAfterBrokerLogin;
int(__cdecl* BrokerMessage)(const string Text) = NULL;
int(__cdecl* BrokerProgress)(intptr_t Progress) = NULL;
int mCurrentTickNdx;
int mTickVolume;
int mPrevMinutes;
float mHighPrice;
float mLowPrice;
std::string mCachePath;
struct SerialArrays mSerialArrays;
#pragma endregion


#pragma region Functions
// read in and decompress data of 1 day in cTrader tick data T1 format
// Only t1 included bid and ask ticks
// Bars only hav bid data
std::string ReadCTraderDayV2(
   const std::string& symbolFile,
   const std::string& dateString)
{
   std::string fileName = symbolFile + "\\" + dateString + ".zticks";
   if (!std::filesystem::exists(fileName))
   {
      return "Tickdata file " + fileName + " not found";
   }

   // Read the compressed file into a byte array
   std::ifstream file(fileName, std::ios_base::in | std::ios_base::binary);
   if (!file)
   {
      return "Error opening file " + fileName;
   }

   // Decompress the file
   std::vector<char> compressedData((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
   std::vector<char> decompressedData;

   z_stream strm = {};
   strm.total_in = strm.avail_in = (uLong)compressedData.size();
   strm.total_out = strm.avail_out = (uLong)compressedData.size() * 16; // Estimated size
   strm.next_in = (Bytef*)compressedData.data();
   decompressedData.resize(strm.total_out);
   strm.next_out = (Bytef*)decompressedData.data();

   inflateInit2(&strm, 16 + MAX_WBITS);
   int ret = inflate(&strm, Z_FINISH);
   if (ret != Z_STREAM_END)
   {
      inflateEnd(&strm);
      return "Decompresson error";
   }
   inflateEnd(&strm);
   decompressedData.resize(strm.total_out);
   mSerialArrays.Tick2dt.resize(strm.total_out / 24);
   mSerialArrays.Tick2Bid.resize(strm.total_out / 24);
   mSerialArrays.Tick2Ask.resize(strm.total_out / 24);

   for (size_t sourceNdx = 0, targetNdx = 0;
      sourceNdx < decompressedData.size();
      targetNdx++)
   {
      mSerialArrays.Tick2dt[targetNdx] = *reinterpret_cast<int64_t*>(&decompressedData[sourceNdx]); // msecs since epoc
      sourceNdx += 8;
      uint32_t bid = *reinterpret_cast<int64_t*>(&decompressedData[sourceNdx]);   // tick size corrected bid
      sourceNdx += 8;
      uint32_t ask = *reinterpret_cast<int64_t*>(&decompressedData[sourceNdx]);   // tick size corrected ask
      sourceNdx += 8;

      mSerialArrays.Tick2Bid[targetNdx] = (bid == 0) ? (targetNdx == 0 ? ask : mSerialArrays.Tick2Bid[targetNdx - 1]) : bid;
      mSerialArrays.Tick2Ask[targetNdx] = (ask == 0) ? (targetNdx == 0 ? bid : mSerialArrays.Tick2Ask[targetNdx - 1]) : ask;
   }

   return "";  // Empty string if successful
}


DLLFUNC int BrokerOpen(string Name, FARPROC fpMessage, FARPROC fpProgress)
{
   strcpy_s(Name, 32, "cTraderCache");
   (FARPROC&)BrokerMessage = fpMessage;
   (FARPROC&)BrokerProgress = fpProgress;
   return 2;   // Version number
}


DLLFUNC int BrokerLogin(
   string User,       // User name
   string Password,   // Password
   string Type,       // Real vs. Demo as defined in Accounts.csv
   string Accounts)   // not used
{
   // logging out ?
   if (0 == User)
      return 1;

   // build path to cTraders backtesting cache data
   // User name and password are "missused" as broker name and real/demo-magicNumer identification
   // $(APPDATA)\Roaming\Spotware\Cache\User\BacktestingCache\V1\Password
   mCachePath = std::getenv("APPDATA");
   mCachePath.append("\\Spotware\\Cache\\");
   mCachePath.append(User);
   mCachePath.append("\\BacktestingCache\\V1\\");
   mCachePath.append(Password);

   // init some vars
   mCurrentTickNdx = -1;
   mPrevMinutes = 0;
   mIs1stAfterBrokerLogin = true;

   // test and return if directory exits
   return std::filesystem::is_directory(mCachePath) ? 1 : 0;
}


// The ticks array is filled in reverse order from tEnd on until either the tick time reaches tStart 
// or the number of ticks reaches nTicks, whichever happens first.The most recent tick, closest to tEnd, 
// is at the start of the array.
DLLFUNC int BrokerHistory2(string Symbol, DATE Start, DATE End, int TickMinutes, int NTicks, T6* Ticks)
{
#ifdef _DEBUG
   // for debugging purposes only
   string startDt = strdate("%Y%m%d %H:%M:%S.", Start);
   string endDt = strdate("%Y%m%d %H:%M:%S.", End);
   std::string sDt = "";
#endif
   std::string error;
   double tickSize = 1e-5; // Support: Must get ticksize from assets file. How to get it? 
   // best would be if BrokerAsset would be called before BrokerHistory2
   // 2nd best file access to Zorro\History\Assets... file :-(

   int tickCount = 0;   // Counting ticks transfered via T6* Ticks
   mPrevMinutes = 0x7fffffff; // int max value; indicate 1st run in BrokerHistory2
   for (;; mCurrentTickNdx--) // cTrader ticks are counted down
   {
      // no more data, reload a new day
      if (mCurrentTickNdx < 0)
         // search from End down to Start to find the 1st existing day
         for (DATE endRun = mIs1stAfterBrokerLogin ? End : End - 1; endRun >= Start - 10; endRun -= 1)
         {
            string sEndRun = strdate("%Y%m%d", endRun);
            error = ReadCTraderDayV2(mCachePath + "\\" + Symbol + "\\t1", sEndRun);
            if ("" == error)
            {
               // Found a valid day; set mCurrentTickNdx to the last tick
               mCurrentTickNdx = (int)mSerialArrays.Tick2dt.size() - 1;
               break;
            }
         }

      mIs1stAfterBrokerLogin = false; // first run done after broker login done

      // no more data found 
      if (-1 == mCurrentTickNdx)
         return 0;   // Support: What to do if there are no more data on the way down to Start?

      // cTrader uses unix epoc format for UTC dateTime information (milliseconds since 1.1.1970)
      // 25569.0 = 1.1.1970 unix epoc start in DATE format; 86400000.0 = msecs per day
      DATE dt = 25569.0 + mSerialArrays.Tick2dt[mCurrentTickNdx] / 86400000.0;
#ifdef DEBUG
      sDt = strdate("%Y%m%d %H:%M:%S.", dt);   // debug check
#endif
      // TickMinutes values are 0 for single price ticks(T1 data; optional), 
      // 1 for one minute(M1) historical data, 
      // Support: (How) Can there also be bars requested with higher minute size (TickMinutes > 1)?
      if (0 == TickMinutes)
      {
         // Single ticks to be filled with the !!!ask!!! prices 
         // Spread goes into fVal
         Ticks->time = dt; // UTC timestamp of the !!!close!!!, DATE format
         Ticks->fOpen = Ticks->fHigh = Ticks->fLow = Ticks->fClose = mSerialArrays.Tick2Ask[mCurrentTickNdx] * tickSize;
         Ticks->fVal = (mSerialArrays.Tick2Ask[mCurrentTickNdx] - mSerialArrays.Tick2Bid[mCurrentTickNdx]) * tickSize;
         Ticks->fVol = 1;

         // done if NTicks ticks read OR if last tick read is < Start
         if (++tickCount >= NTicks || Ticks->time <= Start)
            break;

         Ticks++; // inc Zorro's tick pointer
      }
      else
      {
         // n minute bars
         // cTrader's bid and ask values are in int format calculated as bid = (int)(bid / ticksize)
         int epocMinutes = mSerialArrays.Tick2dt[mCurrentTickNdx] / (TickMinutes * 60000);
         float ask = mSerialArrays.Tick2Ask[mCurrentTickNdx] * tickSize;
         float bid = mSerialArrays.Tick2Bid[mCurrentTickNdx] * tickSize;
         if (epocMinutes < mPrevMinutes)
         {
            if (0x7fffffff != mPrevMinutes)  // do not inc Ticks and tickCount 1st time after call to BrokerHistory2
            {
               Ticks++;       // inc Zorro's tick pointer
               tickCount++;   // count this bar
            }

            // init bar with close time and close price
            Ticks->time = dt; // UTC timestamp of the !!!close!!! in DATE format
            Ticks->fOpen = Ticks->fClose = Ticks->fHigh = Ticks->fLow = ask;
            Ticks->fVol = mTickVolume = 0;

            if (tickCount >= NTicks || Ticks->time <= Start)
               break;
         }

         // build bar backward
         Ticks->fHigh = max(Ticks->fHigh, ask);
         Ticks->fLow = min(Ticks->fLow, ask);
         Ticks->fOpen = ask;
         Ticks->fVal = ask - bid;
         Ticks->fVol = ++mTickVolume;

         mPrevMinutes = epocMinutes;   // make current minutes number to previous minutes number
      }
   }

   return tickCount;
}


DLLFUNC string BrokerRequest(const string Path, const string Method, const string Data)
{
   return NULL;
}


DLLFUNC int BrokerAsset(string Symbol, double* pPrice, double* pSpread,
   double* pVolume, double* pPip, double* pPipCost, double* pMinAmount,
   double* pMargin, double* pRollLong, double* pRollShort)
{
   return 0;
}


DLLFUNC int BrokerAccount(string AccountId, double* pdBalance, double* pdTradeVal, double* pdMarginVal)
{
   return 0;
}


DLLFUNC int BrokerTrade(int nTradeID, double* pOpen, double* pClose, double* pRoll, double* pProfit)
{
   return 0;
}


DLLFUNC int BrokerBuy2(string Symbol, int Volume, double StopDist, double Limit, double* pPrice, int* pFill)
{
   //BrokerMessage("\nBrokerBuy2\n");
   return 0;
}


DLLFUNC double BrokerCommand(int Mode, intptr_t Parameter)
{
   switch (Mode)
   {
   case GET_MAXTICKS:
      return 500;       // chunks of 500 ticks 

   case SET_HWND:       // Zorro window handle
      return 0;

   case SET_FUNCTIONS:  // no callback functions needed
      return 0;

   case GET_MAXREQUESTS:
      return 0;         // No limit in request speed since reading data from files

   /*
   SET_PRICETYPE:
      0 - Broker default (ask/bid if available, otherwise last trade price);
      1 - enforce ask/bid quotes;
      2 - enforce last trade price;
      3 - special;
      4 - no price requests;
      8 - fast price requests: ask, bid, or trade, whatever received first.
      The spread is normally only updated when ask/bid quotes are returned.
   */
   case SET_PRICETYPE:
      G.PriceType = Parameter;
      return 1;

      /*
      case SET_SERVER:
         return 0;



      case GET_BROKERZONE:
         return 0;

      case GET_CALLBACK:
         return 0;

      case SET_EXCHANGES:
         return 0;

      case GET_EXCHANGES:
         return 0;

      case GET_LOCK:
         return -1;

      case GET_HEARTBEAT:
         return 0;
         */
         // org
   case GET_COMPLIANCE:
      return 2;

   case SET_DIAGNOSTICS:
      G.Diag = Parameter;
      return 1;

   case SET_AMOUNT:
      G.Unit = *(double*)Parameter;
      if (G.Unit <= 0.) G.Unit = 0.00001;
      return 1;

   case GET_UUID:
      strcpy_s((string)Parameter, 256, G.Uuid);
      return 1;
   case SET_UUID:
      strcpy_s(G.Uuid, (string)Parameter);
      return 1;
   case SET_VOLTYPE:
      G.VolType = Parameter;
      return 1;
   case SET_ORDERTYPE:
      G.OrderType = Parameter;
      return Parameter & 3;

   case GET_POSITION:
   {
      double Value = 0;
      return Value / min(1., G.Unit);
   }

   case DO_CANCEL:
   {
      //string Response;
      if (Parameter == 0)
      {
         //Response = send("orders/open", 1, "DELETE");
         //if (Response) return 1;
      }
      else
      {
         sprintf_s(G.Url, "orders/%s", G.Uuid);
         //Response = send(G.Url, 1, "DELETE");
         //if (Response) return 1;
         //var Quantity = strvar(Response,"fillQuantity",0);
         //return Quantity;
      }
      return 0;
   }
   }
   return 0.;
}
#pragma endregion


// end of file

