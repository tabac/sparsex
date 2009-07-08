#include <iostream>
#include <cassert>

#include "llvm/Analysis/Verifier.h"
#include "llvm/ExecutionEngine/ExecutionEngine.h"
#include "llvm/ModuleProvider.h"

#include "spm.h"
#include "csx.h"
#include "drle.h"

#include "llvm_jit_help.h"

extern "C" {
#include "../spmv_method.h"
}


namespace csx {

} // csx namespace end

using namespace csx;
using namespace llvm;

class CsxJit {
public:
	CsxManager *CsxMg;
	Module *M;
	IRBuilder<> *Bld;
	ModuleProvider *MP;
	ExecutionEngine *JIT;

	Function *UlGet;
	Function *DecodeF;
	Function *SpmvF;
	Function *FailF;
	Function *PrintYXV;
	Function *AlignF;
	Function *TestBitF;

	Value *Zero8;
	Value *Zero32;
	Value *Zero64;
	Value *One8;
	Value *One32;
	Value *One64;
	Value *Three64;

	Value *YrPtr;
	Value *MyxPtr;
	Value *Xptr;
	Value *Yptr;
	Value *YindxPtr;
	Value *Vptr;
	Value *CtlPtr;
	Value *SizePtr;
	Value *FlagsPtr;

	Annotations annotations;

	CsxJit(CsxManager *CsxMg);

	void doPrint();
	void doIncV();
	void doDeltaAddMyx(int delta_bytes);

	void doNewRowHook();
	void doBodyHook();
	void doHooks();
	void *doJit();

	void DeltaCase(BasicBlock *BB,        // case
	               BasicBlock *BB_lentry, // loop entry
	               BasicBlock *BB_lbody,  // loop body
	               BasicBlock *BB_exit,   // final exit
	               int delta_bytes);

	void HorizCase(BasicBlock *BB,
	               BasicBlock *BB_lbody,
	               BasicBlock *BB_lexit,
                   BasicBlock *BB_exit,
                   int delta_size);

	void VertCase(BasicBlock *BB,
                  BasicBlock *BB_lbody,
                  BasicBlock *BB_exit,
                  int delta_size);

	void DiagCase(BasicBlock *BB,
                  BasicBlock *BB_lbody,
                  BasicBlock *BB_exit,
                  int delta_size);
};

CsxJit::CsxJit(CsxManager *_ctl_mg) : CsxMg(_ctl_mg)
{
	this->M = ModuleFromFile("csx_llvm_tmpl.llvm.bc");
	this->Bld = new IRBuilder<>();

	this->annotations.update(M);

	//this->DecodeF = M->getFunction("ctl_decode_template");
	this->SpmvF = M->getFunction("csx_spmv_template");
	this->PrintYXV = M->getFunction("print_yxv");
	this->FailF = M->getFunction("fail");
	this->AlignF = M->getFunction("align_ptr");
	this->TestBitF = M->getFunction("test_bit");
	this->UlGet = M->getFunction("ul_get");

	this->YrPtr = annotations.getValue("spmv::yr");
	this->MyxPtr = annotations.getValue("spmv::myx");
	this->Xptr = annotations.getValue("spmv::x");
	this->Yptr = annotations.getValue("spmv::y");
	this->YindxPtr = annotations.getValue("spmv::y_indx");
	this->Vptr = annotations.getValue("spmv::v");
	this->CtlPtr = annotations.getValue("spmv::ctl");
	this->SizePtr = annotations.getValue("spmv::size");
	this->FlagsPtr = annotations.getValue("spmv::flags");

	this->Zero8 = ConstantInt::get(Type::Int8Ty, 0);
	this->Zero32 = ConstantInt::get(Type::Int32Ty, 0);
	this->Zero64 = ConstantInt::get(Type::Int64Ty, 0);
	this->One8 = ConstantInt::get(Type::Int8Ty, 1);
	this->One32 = ConstantInt::get(Type::Int32Ty, 1);
	this->One64 = ConstantInt::get(Type::Int64Ty, 1);
	this->Three64 = ConstantInt::get(Type::Int64Ty, 3);
}

void CsxJit::doNewRowHook()
{
	BasicBlock *BB, *BB_next;
	Value *v;

	// new row
	BB = llvm_hook_newbb(M, "__new_row_hook", &BB_next);
	Bld->SetInsertPoint(BB);
	if (!CsxMg->row_jmps){
		v = Bld->CreateLoad(YindxPtr, "y_indx");
		v = Bld->CreateAdd(v, One64, "y_indx_inc");
		Bld->CreateStore(v, YindxPtr);
		Bld->CreateBr(BB_next);
	} else {
		BasicBlock *BB_rjmp, *BB_rend;
		Value *RJmpBit, *Yindx, *Test;
		Value *Ul;
		PHINode *YindxAdd;

		BB_rjmp = BasicBlock::Create("rjmp", BB->getParent(), BB_next);
		BB_rend = BasicBlock::Create("rend", BB->getParent(), BB_next);
		RJmpBit = ConstantInt::get(Type::Int32Ty, CTL_RJMP_BIT);

		Yindx = Bld->CreateLoad(YindxPtr, "y_indx");
		Test = Bld->CreateCall2(TestBitF, FlagsPtr, RJmpBit);
		Test = Bld->CreateICmpEQ(Test, Zero32, "bit_test");
		Bld->CreateCondBr(Test, BB_rend, BB_rjmp);

		Bld->SetInsertPoint(BB_rjmp);
		Ul = Bld->CreateCall(UlGet, CtlPtr);
		Bld->CreateBr(BB_rend);

		// common end
		Bld->SetInsertPoint(BB_rend);
		YindxAdd = Bld->CreatePHI(Type::Int64Ty, "yindx_add");
		YindxAdd->addIncoming(One64, BB);
		YindxAdd->addIncoming(Ul, BB_rjmp);

		v = Bld->CreateAdd(YindxAdd, Yindx);
		Bld->CreateStore(v, YindxPtr);
		Bld->CreateBr(BB_next);
	}
}

#if 0
void CsxJit::HorizCase(BasicBlock *BB,
                       BasicBlock *BB_lbody, BasicBlock *BB_lexit,
                       BasicBlock *BB_exit,
                       int delta_size)
{
	Value *Size, *Delta, *Xindx0, *XindxAdd, *Yindx, *NextCnt, *Test;
	PHINode *Xindx, *Cnt;

	Delta = ConstantInt::get(Type::Int64Ty, delta_size);

	Bld->SetInsertPoint(BB);
	Size = Bld->CreateLoad(SizePtr, "size");
	Xindx0 = Bld->CreateLoad(XindxPtr);
	Yindx = Bld->CreateLoad(YindxPtr);
	Bld->CreateBr(BB_lbody);

	// Body
	Bld->SetInsertPoint(BB_lbody);
	Cnt = Bld->CreatePHI(Type::Int8Ty, "cnt");
	Xindx = Bld->CreatePHI(Type::Int64Ty, "xindx");
	Bld->CreateCall2(PrintYX, Yindx, Xindx);
	XindxAdd = Bld->CreateAdd(Xindx, Delta);
	NextCnt = Bld->CreateAdd(Cnt, One8, "next_cnt");
	Test = Bld->CreateICmpEQ(NextCnt, Size, "cnt_test");
	Bld->CreateCondBr(Test, BB_lexit, BB_lbody);

	Cnt->addIncoming(Zero8, BB);
	Cnt->addIncoming(NextCnt, BB_lbody);

	Xindx->addIncoming(Xindx0, BB);
	Xindx->addIncoming(XindxAdd, BB_lbody);

	// Exit
	Bld->SetInsertPoint(BB_lexit);
	Bld->CreateStore(Xindx, XindxPtr);
	Bld->CreateBr(BB_exit);
}

void CsxJit::VertCase(BasicBlock *BB,
                      BasicBlock *BB_lbody,
                      BasicBlock *BB_exit,
                      int delta_size)
{
	Value *Size, *Delta, *Yindx0, *YindxAdd, *Xindx, *NextCnt, *Test;
	PHINode *Yindx, *Cnt;

	Delta = ConstantInt::get(Type::Int64Ty, delta_size);

	Bld->SetInsertPoint(BB);
	Size = Bld->CreateLoad(SizePtr, "size");
	Xindx = Bld->CreateLoad(XindxPtr);
	Yindx0 = Bld->CreateLoad(YindxPtr);
	Bld->CreateBr(BB_lbody);

	// Body
	Bld->SetInsertPoint(BB_lbody);
	Cnt = Bld->CreatePHI(Type::Int8Ty, "cnt");
	Yindx = Bld->CreatePHI(Type::Int64Ty, "yindx");
	Bld->CreateCall2(PrintYX, Yindx, Xindx);
	YindxAdd = Bld->CreateAdd(Yindx, Delta);
	NextCnt = Bld->CreateAdd(Cnt, One8, "next_cnt");
	Test = Bld->CreateICmpEQ(NextCnt, Size, "cnt_test");
	Bld->CreateCondBr(Test, BB_exit, BB_lbody);

	Cnt->addIncoming(Zero8, BB);
	Cnt->addIncoming(NextCnt, BB_lbody);

	Yindx->addIncoming(Yindx0, BB);
	Yindx->addIncoming(YindxAdd, BB_lbody);
}

void CsxJit::DiagCase(BasicBlock *BB,
                      BasicBlock *BB_lbody,
                      BasicBlock *BB_exit,
                      int delta_size)
{
	Value *Size, *Delta, *Test;
	PHINode *Xindx, *Yindx, *Cnt;
	Value *Xindx0, *Yindx0;
	Value *XindxAdd, *YindxAdd, *NextCnt;

	Delta = ConstantInt::get(Type::Int64Ty, delta_size);

	Bld->SetInsertPoint(BB);
	Size = Bld->CreateLoad(SizePtr, "size");
	Xindx0 = Bld->CreateLoad(XindxPtr);
	Yindx0 = Bld->CreateLoad(YindxPtr);
	Bld->CreateBr(BB_lbody);

	// Body
	Bld->SetInsertPoint(BB_lbody);
	Cnt = Bld->CreatePHI(Type::Int8Ty, "cnt");
	Yindx = Bld->CreatePHI(Type::Int64Ty, "yindx");
	Xindx = Bld->CreatePHI(Type::Int64Ty, "xindx");
	Bld->CreateCall2(PrintYX, Yindx, Xindx);
	YindxAdd = Bld->CreateAdd(Yindx, Delta);
	XindxAdd = Bld->CreateAdd(Xindx, Delta);
	NextCnt = Bld->CreateAdd(Cnt, One8, "next_cnt");
	Test = Bld->CreateICmpEQ(NextCnt, Size, "cnt_test");
	Bld->CreateCondBr(Test, BB_exit, BB_lbody);

	Cnt->addIncoming(Zero8, BB);
	Cnt->addIncoming(NextCnt, BB_lbody);

	Xindx->addIncoming(Xindx0, BB);
	Xindx->addIncoming(XindxAdd, BB_lbody);

	Yindx->addIncoming(Yindx0, BB);
	Yindx->addIncoming(YindxAdd, BB_lbody);
}
#endif

void CsxJit::doPrint()
{
	Value *X, *Myx, *Xindx;
	Myx = Bld->CreateLoad(MyxPtr);
	Myx = Bld->CreatePtrToInt(Myx, Type::Int64Ty, "myx_int");
	X = Bld->CreateLoad(Xptr);
	X = Bld->CreatePtrToInt(X, Type::Int64Ty, "x_int");
	Xindx = Bld->CreateSub(Myx, X);
	Xindx = Bld->CreateAShr(Xindx, Three64);

	Value *Yindx;
	Yindx = Bld->CreateLoad(YindxPtr);

	Value *V;
	V = Bld->CreateLoad(Bld->CreateLoad(Vptr));

	Bld->CreateCall3(PrintYXV, Yindx, Xindx, V);
}

void CsxJit::doIncV()
{
	Value *V = Bld->CreateLoad(Vptr);
	Value *newV = Bld->CreateGEP(V, One64);
	Bld->CreateStore(newV, Vptr);
}

void CsxJit::doDeltaAddMyx(int delta_bytes)
{
	Function *F;

	switch (delta_bytes){
		case 1:
		F = M->getFunction("u8_get");
		break;

		case 2:
		F = M->getFunction("u16_get");
		break;

		case 4:
		F = M->getFunction("u32_get");
		break;

		case 8:
		F = M->getFunction("u64_get");
		break;

		default:
		assert(false);
	}

	Value *Myx = Bld->CreateLoad(MyxPtr, "myx");
	Value *MyxAdd = Bld->CreateCall(F, CtlPtr, "myx_add");
	Value *newMyx = Bld->CreateGEP(Myx, MyxAdd, "newmyx");
	Bld->CreateStore(newMyx, MyxPtr);
}

void CsxJit::DeltaCase(BasicBlock *BB,
                       BasicBlock *BB_entry, BasicBlock *BB_body,
                       BasicBlock *BB_exit,
                       int delta_bytes)
{
	Value *Align, *Size, *Test, *NextCnt;
	PHINode *Cnt;

	Bld->SetInsertPoint(BB);
	// align ctl
	if (delta_bytes > 1){
		Align = ConstantInt::get(Type::Int32Ty,delta_bytes);
		Bld->CreateCall2(AlignF, CtlPtr, Align);
	}
	Size = Bld->CreateLoad(SizePtr, "size");
	Bld->CreateBr(BB_entry);

	// Entry
	Bld->SetInsertPoint(BB_entry);
	doPrint();
	doIncV();
	Test = Bld->CreateICmpUGT(Size, One8);
	Bld->CreateCondBr(Test, BB_body, BB_exit);

	// Body
	Bld->SetInsertPoint(BB_body);
	Cnt = Bld->CreatePHI(Type::Int8Ty, "cnt");
	doDeltaAddMyx(delta_bytes);
	NextCnt = Bld->CreateAdd(Cnt, One8, "next_cnt");
	doPrint();
	doIncV();
	Test = Bld->CreateICmpEQ(NextCnt, Size, "cnt_test");
	Bld->CreateCondBr(Test, BB_exit, BB_body);

	Cnt->addIncoming(One8, BB_entry);
	Cnt->addIncoming(NextCnt, BB_body);
}

void CsxJit::doBodyHook()
{
	BasicBlock *BB, *BB_next, *BB_default, *BB_case;
	Value *PatternMask;
	Value *v;

	BB = llvm_hook_newbb(M, "__body_hook", &BB_next);

	// get pattern for switch instruction
	Bld->SetInsertPoint(BB);
	PatternMask = ConstantInt::get(Type::Int8Ty, CTL_PATTERN_MASK);
	v = Bld->CreateLoad(FlagsPtr, "flags");
	v = Bld->CreateAnd(PatternMask, v, "pattern");

	// switch default block (call the fail function)
	BB_default = BasicBlock::Create("default", BB->getParent(), BB_next);
	Bld->SetInsertPoint(BB_default);
	Bld->CreateCall(FailF);
	Bld->CreateBr(BB_next);

	// switch instruction
	SwitchInst *Switch;
	Bld->SetInsertPoint(BB);
	std::cerr << "Constructing switch with " << CsxMg->patterns.size() << " cases\n";
	Switch = Bld->CreateSwitch(v, BB_default, CsxMg->patterns.size());

	// Fill up switch, by iterating given patterns
	CsxManager::PatMap::iterator pat_i = CsxMg->patterns.begin();
	BasicBlock *BB_lentry, *BB_lbody, *BB_lexit;
	for ( ; pat_i !=  CsxMg->patterns.end(); ++pat_i){
		std::cerr << "pat:" << pat_i->first << " flag:" << (int)pat_i->second.flag << "\n";

		// Alocate case + loop BBs
		BB_case = BasicBlock::Create("case", BB->getParent(), BB_default);
		switch (pat_i->first){
			// Deltas
			case 8: case 16: case 32: case 64:
			BB_lentry = BasicBlock::Create("lentry", BB->getParent(), BB_default);
			BB_lbody = BasicBlock::Create("lbody", BB->getParent(), BB_default);
			DeltaCase(BB_case,
			          BB_lentry, BB_lbody,
			          BB_next,
			          pat_i->first / 8);
			break;

			#if 0
			// Horizontal
			case 10000 ... 19999:
			BB_lbody = BasicBlock::Create("lbody", BB->getParent(), BB_default);
			BB_lexit = BasicBlock::Create("lexit", BB->getParent(), BB_default);
			HorizCase(BB_case,
			          BB_lbody, BB_lexit,
			          BB_next,
			          pat_i->first -10000);
			break;

			// Vertical
			case 20000 ... 29999:
			BB_lbody = BasicBlock::Create("lbody", BB->getParent(), BB_default);
			VertCase(BB_case,
			         BB_lbody,
			         BB_next,
			         pat_i->first -20000);
			break;

			// Diagonal
			case 30000 ... 39999:
			BB_lbody = BasicBlock::Create("lbody", BB->getParent(), BB_default);
			DiagCase(BB_case,
			         BB_lbody,
			         BB_next,
			         pat_i->first -30000);
			break;
			#endif

			// rdiag
			case 40000 ... 49999:
			default:
			assert(false);
			break;
		}

		Switch->addCase(
			ConstantInt::get(Type::Int8Ty, pat_i->second.flag),
			BB_case
		);
	}
}

void CsxJit::doHooks()
{
	doNewRowHook();
	doBodyHook();
}

void *CsxJit::doJit()
{
	verifyModule(*M, AbortProcessAction, 0);
	ModuleToFile(M, "M.llvm.bc");
	//doOptimize(M);
	//M->dump();
	std::cerr << "Generating Function\n";
	std::string Error;
	MP = new  ExistingModuleProvider(M);
	JIT = ExecutionEngine::createJIT(MP, &Error);
	if (!JIT){
		std::cerr << "ExectionEngine::createJIT:" << Error << "\n";
		exit(1);
	}
	return JIT->getPointerToFunction(SpmvF);
}


void doEncode(SpmIdx *Spm)
{
	DRLE_Manager *DrleMg;
	SpmIterOrder type;

	// 255-1 is because we need drle with <= 255-1 size, so that
	// patterns with jmps have 255 elements
	DrleMg = new DRLE_Manager(Spm, 4, 255-1);
	DrleMg->genAllStats();
	DrleMg->outStats(std::cerr);

	type = DrleMg->chooseType();
	if (type == NONE)
		return;
	std::cerr << "Encode for " << SpmTypesNames[type] << std::endl;
	Spm->Transform(type);
	DrleMg->Encode();
	Spm->Transform(HORIZONTAL);
	//Spm->Print(std::cerr);
}

int main(int argc, char **argv)
{
	SpmIdx *Spm;
	CsxManager *CsxMg;
	CsxJit *Jit;
	csx_double_t *csx;
	uint64_t ctl_size;

	if (argc < 2){
		std::cerr << "Usage: " << argv[0] << " <mmf_file>\n";
		exit(1);
	}

	Spm = loadMMF_mt(argv[1], 1);
	Spm->Print(std::cerr);
	//doEncode(Spm);

	CsxMg = new CsxManager(Spm);
	Jit = new CsxJit(CsxMg);
	csx = CsxMg->mkCsx();

	Jit->doHooks();
	spmv_double_fn_t *fn = (spmv_double_fn_t *)Jit->doJit();
	fn(csx, NULL, NULL);

	//delete Spm;
	delete CsxMg;

	return 0;
}
