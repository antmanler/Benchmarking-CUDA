/*! \file PTXKernel.cpp
	\author Gregory Diamos <gregory.diamos@gatech>
	\date Thursday September 17, 2009
	\brief The header file for the PTXKernel class
*/

#ifndef PTX_KERNEL_H_INCLUDED
#define PTX_KERNEL_H_INCLUDED

#include <ocelot/ir/interface/PTXKernel.h>
#include <ocelot/ir/interface/ControlFlowGraph.h>

#include <hydrazine/interface/Version.h>
#include <hydrazine/implementation/debug.h>

#ifdef REPORT_BASE
#undef REPORT_BASE
#endif

#define REPORT_BASE 0

namespace ir
{
	PTXKernel::PTXKernel( const std::string& name, bool isFunction,
		const ir::Module* module ) :
		Kernel( Instruction::PTX, name, isFunction, module )
	{
		_cfg = new ControlFlowGraph;
	}

	PTXKernel::PTXKernel( PTXStatementVector::const_iterator start,
		PTXStatementVector::const_iterator end, bool function) : 
		Kernel( Instruction::PTX, "", function )
	{
		// get parameters/locals, extract kernel name
		for( PTXStatementVector::const_iterator it = start; it != end; ++it ) 
		{
			if( (*it).directive == PTXStatement::Param )
			{
				parameters.push_back( Parameter( *it ) );
			}
			else if( (*it).directive == PTXStatement::Local
				|| (*it).directive == PTXStatement::Shared )
			{
				locals.insert( std::make_pair( ( *it ).name, Local( *it ) ) );
			}
			else if( (*it).directive == PTXStatement::Entry )
			{
				name = (*it).name;
			}
		}
		_cfg = new ControlFlowGraph;
		constructCFG( *_cfg, start, end );
		assignRegisters( *_cfg );
	}

	PTXKernel::PTXKernel( const PTXKernel& kernel ) : Kernel( kernel )
	{
		
	}

	const PTXKernel& PTXKernel::operator=(const PTXKernel &kernel) 
	{
		if( &kernel == this ) return *this;
		
		Kernel::operator=(kernel);
		_function = kernel.function();

		return *this;	
	}

	void PTXKernel::addUsedRegister(RegisterTypeMap& regMap, ir::PTXOperand& op) const {
		ir::PTXOperand::AddressMode operandAddressMode = op.addressMode;
		if ((operandAddressMode == ir::PTXOperand::Register) || (operandAddressMode == ir::PTXOperand::Indirect)) {
			//assertM(op.identifier != "", "addUsedRegister(): operand type " + op.toString(operandAddressMode) + "identifier is empty");
			analysis::DataflowGraph::RegisterId operandId = op.reg;
			ir::PTXOperand::DataType operandType = op.type;

			// not add kernel parameters as registers
			//if (!hasParameter(operandId)) {

			op.identifier = "";

				// sometimes registers are used as operands with the wrong type,
				// so avoid generating declarations based on operand registers
				if (regMap.find(operandId) == regMap.end()) {
					// not found, add it
					regMap[operandId] = operandType;
				}
			//}

		}
	}

	PTXKernel::RegisterTypeMap PTXKernel::getReferencedRegistersWithoutDFG() const {
		RegisterTypeMap regMap;

		ir::ControlFlowGraph::const_iterator block = _cfg->begin();
		for (; block != _cfg->end(); ++block) {

			ir::ControlFlowGraph::BasicBlock::InstructionList::const_iterator instr = block->instructions.begin();
			for (; instr != block->instructions.end(); ++instr) {
				ir::PTXInstruction* ptxInst = static_cast<ir::PTXInstruction*>(*instr);

				// Get the destination operand of each instruction
				ir::PTXOperand::AddressMode operandAddressMode = ptxInst->d.addressMode;
				if (ptxInst->isUsingOperand(ir::PTXInstruction::OperandD)) {
					if ((ptxInst->opcode == ir::PTXInstruction::Bra)
							|| (ptxInst->opcode == ir::PTXInstruction::St)) {
						addUsedRegister(regMap, ptxInst->d);
					} else if ((ptxInst->d.addressMode == ir::PTXOperand::Register)
							|| (ptxInst->d.addressMode == ir::PTXOperand::Indirect)) {

						/*std::stringstream ss;
						ss << "getReferencedRegistersWithoutDFG(): operand D identifier is empty, code: ";
						ss << ptxInst->toString();
						ss << " reg field is " << ptxInst->d.reg;
						assertM(ptxInst->d.identifier != "", ss.str());*/

						ptxInst->d.identifier = "";

						analysis::DataflowGraph::RegisterId operandId = ptxInst->d.reg;
						ir::PTXOperand::DataType operandType = ptxInst->d.type;

						regMap[operandId] = operandType;
					}
				}

				// Check operands (some codes use registers without initializing them)
				addUsedRegister(regMap, ptxInst->a);
				addUsedRegister(regMap, ptxInst->b);
				addUsedRegister(regMap, ptxInst->c);

				// If instruction is a SetP, look for pq too
				if (ptxInst->opcode == ir::PTXInstruction::SetP) {
					operandAddressMode = ptxInst->pq.addressMode;

					// pq predicates must be registers
					if (operandAddressMode == ir::PTXOperand::Register) {
						analysis::DataflowGraph::RegisterId operandId = ptxInst->pq.reg;
						ir::PTXOperand::DataType operandType = ptxInst->pq.type;

						regMap[operandId] = operandType;
					}
				}

			} // for each instruction
		} // for each basic block

		return regMap;
	}

	PTXKernel::RegisterVector PTXKernel::getReferencedRegisters() const
	{
		report( "Getting list of all referenced registers" );				

		typedef std::unordered_set< analysis::DataflowGraph::RegisterId > 
			RegisterSet;

		RegisterSet encountered;
		RegisterSet predicates;
		RegisterSet addedRegisters;
		RegisterVector regs;
		
		for( ControlFlowGraph::const_iterator block = cfg()->begin(); 
			block != cfg()->end(); ++block )
		{
			report( " For block " << block->label );
						
			for( ControlFlowGraph::InstructionList::const_iterator 
				instruction = block->instructions.begin(); 
				instruction != block->instructions.end(); ++instruction )
			{
				report( "  For instruction " << (*instruction)->toString() );
				
				bool detailed = false;
				if ((*instruction)->toString() == "mov.pred %p187, %p186") {
					detailed = true;
				}
				
				const ir::PTXInstruction& ptx = static_cast<
					const ir::PTXInstruction&>(**instruction);
				
				if( ptx.opcode == ir::PTXInstruction::St ) continue;
				
				const ir::PTXOperand* operands[] = {&ptx.pq, &ptx.d, &ptx.a};

				unsigned int limit = 2;
				
				if( ir::PTXInstruction::St == ptx.opcode )
				{
					limit = 1;
				}
				else if( ir::PTXInstruction::Bfi == ptx.opcode )
				{
					limit = 1;
					operands[ 0 ] = &ptx.d;
				}

				for( unsigned int i = 0; i < 3; ++i )
				{
					const ir::PTXOperand& d = *operands[i];
					
					if( d.addressMode != ir::PTXOperand::Register ) continue;
					
					if( d.type != ir::PTXOperand::pred )
					{
						if( d.array.empty() )
						{
							if( encountered.insert( d.reg ).second )
							{
								report( "   Added %r" << d.reg );
								analysis::DataflowGraph::Register live_reg( 
									d.reg, d.type );
								if (addedRegisters.find(live_reg.id) == addedRegisters.end()) {
									regs.push_back( live_reg );
									addedRegisters.insert(live_reg.id);
								}
							}
						}
						else
						{
							for( PTXOperand::Array::const_iterator 
								operand = d.array.begin(); 
								operand != d.array.end(); ++operand )
							{
								report( "   Added %r" << operand->reg );
								analysis::DataflowGraph::Register live_reg( 
									operand->reg, operand->type );
								if (addedRegisters.find(live_reg.id) == addedRegisters.end()) {
									regs.push_back( live_reg );
									addedRegisters.insert(live_reg.id);
								}
							}
						}
					}
					else
					{
						if( predicates.insert( d.reg ).second )
						{
							report( "   Added %p" << d.reg );
							analysis::DataflowGraph::Register live_reg( 
								d.reg, d.type );
							if (addedRegisters.find(live_reg.id) == addedRegisters.end()) {
								regs.push_back( live_reg );
								addedRegisters.insert(live_reg.id);
							}
						}
					}
				}
			}
		}
		
		// dead code elimintation step?
		
		return regs;
	}

	analysis::DataflowGraph* PTXKernel::dfg() 
	{
		assertM(_cfg != 0, "Must create cfg before building dfg.");
		if(_dfg) return _dfg;
		_dfg = new analysis::DataflowGraph( *_cfg );
		return _dfg;
	}

	const analysis::DataflowGraph* PTXKernel::dfg() const 
	{
		return Kernel::dfg();
	}

	bool PTXKernel::executable() const {
		return false;
	}

	void PTXKernel::constructCFG( ControlFlowGraph &cfg,
		PTXStatementVector::const_iterator kernelStart,
		PTXStatementVector::const_iterator kernelEnd) {
		typedef std::unordered_map< std::string, 
			ControlFlowGraph::iterator > BlockToLabelMap;
		typedef std::vector< ControlFlowGraph::iterator > BlockPointerVector;
	
		BlockToLabelMap blocksByLabel;
		BlockPointerVector branchBlocks;

		ControlFlowGraph::iterator last_inserted_block = cfg.end();
		ControlFlowGraph::iterator block = cfg.insert_block(
			ControlFlowGraph::BasicBlock("", cfg.newId()));
		ControlFlowGraph::Edge edge(cfg.get_entry_block(), block, 
			ControlFlowGraph::Edge::FallThrough);
	
		bool hasExit = false;
	
		unsigned int statementIndex = 0;
		for( ; kernelStart != kernelEnd; ++kernelStart, ++statementIndex ) 
		{
			const PTXStatement &statement = *kernelStart;
		
			if( statement.directive == PTXStatement::Label ) 
			{
				// a label indicates the termination of a previous block
				//
				// This implementation does not store any empty basic blocks.
				if( block->instructions.size() ) {
					//
					// insert old block
					//
					if (edge.type != ControlFlowGraph::Edge::Invalid) {
						cfg.insert_edge(edge);
					}
				
					edge.head = block;
					last_inserted_block = block;
					block = cfg.insert_block(
						ControlFlowGraph::BasicBlock("", cfg.newId()));
					edge.tail = block;
					edge.type = ControlFlowGraph::Edge::FallThrough;
				}
				
				block->label = statement.name;
				assertM( blocksByLabel.count( block->label ) == 0, 
					"Duplicate blocks with label " << block->label )
				blocksByLabel.insert( std::make_pair( block->label, block ) );
			}
			else if( statement.directive == PTXStatement::Instr ) 
			{
				block->instructions.push_back( statement.instruction.clone() );
				
				if (statement.instruction.opcode == PTXInstruction::Bra) 
				{
					last_inserted_block = block;
					// dont't add fall through edges for unconditional branches
					if (edge.type != ControlFlowGraph::Edge::Invalid) {
						cfg.insert_edge(edge);
					}
					edge.head = block;
					branchBlocks.push_back(block);
					block = cfg.insert_block(
						ControlFlowGraph::BasicBlock("", cfg.newId()));
					if (statement.instruction.pg.condition 
						!= ir::PTXOperand::PT) {
						edge.tail = block;
						edge.type = ControlFlowGraph::Edge::FallThrough;
					}
					else {
						edge.type = ControlFlowGraph::Edge::Invalid;
					}
				}
				else if(statement.instruction.opcode == PTXInstruction::Exit)
				{
					assertM(!hasExit, "Duplicate exit block.");
					hasExit = true;
					last_inserted_block = block;
					if (edge.type != ControlFlowGraph::Edge::Invalid) {
						cfg.insert_edge(edge);
					}
					edge.head = block;
					edge.tail = cfg.get_exit_block();
					edge.type = ControlFlowGraph::Edge::FallThrough;
					
					cfg.insert_edge(edge);
					
					block = cfg.insert_block(
						ControlFlowGraph::BasicBlock("", cfg.newId()));
					edge.type = ControlFlowGraph::Edge::Invalid;
				}
				else if( statement.instruction.opcode == PTXInstruction::Call )
				{
					assertM(false, "Unhandled control flow instruction call");
				}
				else if( statement.instruction.opcode == PTXInstruction::Ret )
				{
					last_inserted_block = block;
					if (edge.type != ControlFlowGraph::Edge::Invalid) {
						cfg.insert_edge(edge);
					}
					edge.head = block;
					edge.tail = cfg.get_exit_block();
					edge.type = ControlFlowGraph::Edge::Branch;

					block = cfg.insert_block(
						ControlFlowGraph::BasicBlock("", cfg.newId()));
					edge.type = ControlFlowGraph::Edge::Invalid;
				}
			}
		}

		if (block->instructions.size()) 
		{
			if (edge.type != ControlFlowGraph::Edge::Invalid) {
				cfg.insert_edge(edge);
			}
		}
		else 
		{
			cfg.remove_block(block);
		}

		assertM(hasExit, "No exit point from the kernel found.");

		// go back and add edges for basic blocks terminating in branches
		for( BlockPointerVector::iterator it = branchBlocks.begin();
			it != branchBlocks.end(); ++it ) 
		{
			PTXInstruction& bra = *static_cast<PTXInstruction*>(
				(*it)->instructions.back());
			// skip always false branches
			if( bra.pg.condition == ir::PTXOperand::nPT ) continue;
			
			BlockToLabelMap::iterator labeledBlockIt = 
				blocksByLabel.find( bra.d.identifier );
		
			assertM(labeledBlockIt != blocksByLabel.end(), 
				"undefined label " << bra.d.identifier);
		
			bra.d.identifier = labeledBlockIt->second->label;
			cfg.insert_edge(ControlFlowGraph::Edge(*it, 
				labeledBlockIt->second, ControlFlowGraph::Edge::Branch));
		}
	}

	PTXKernel::RegisterMap PTXKernel::assignRegisters( ControlFlowGraph& cfg ) 
	{
		RegisterMap map;
	
		report( "Allocating registers " );
	
		for (ControlFlowGraph::iterator block = cfg.begin(); 
			block != cfg.end(); ++block) {
			for (ControlFlowGraph::InstructionList::iterator 
				instruction = block->instructions.begin(); 
				instruction != block->instructions.end(); ++instruction) {
				PTXInstruction& instr = *static_cast<PTXInstruction*>(
					*instruction);
				PTXOperand PTXInstruction:: * operands[] = 
				{ &PTXInstruction::a, &PTXInstruction::b, &PTXInstruction::c, 
					&PTXInstruction::d, &PTXInstruction::pg, 
					&PTXInstruction::pq };
		
				report( " For instruction '" << instr.toString() << "'" );
		
				for (int i = 0; i < 6; i++) {
					if ((instr.*operands[i]).addressMode 
						== PTXOperand::Invalid) {
						continue;
					}
					if ((instr.*operands[i]).type == PTXOperand::pred
						&& (instr.*operands[i]).condition == PTXOperand::PT) {
						continue;
					}
					if ((instr.*operands[i]).addressMode == PTXOperand::Register
						|| (instr.*operands[i]).addressMode 
						== PTXOperand::Indirect) {
						if ((instr.*operands[i]).vec != PTXOperand::v1) {
							for (PTXOperand::Array::iterator 
								a_it = (instr.*operands[i]).array.begin(); 
								a_it != (instr.*operands[i]).array.end(); 
								++a_it) {
								RegisterMap::iterator it 
									= map.find(a_it->registerName());

								PTXOperand::RegisterType reg = 0;
								if (it == map.end()) {
									reg = (PTXOperand::RegisterType) map.size();
									map.insert(std::make_pair( 
										a_it->registerName(), reg));
								}
								else {
									reg = it->second;
								}
								a_it->reg = reg;
								report( "  Assigning register " 
									<< a_it->registerName() 
									<< " to " << a_it->reg );
								a_it->identifier.clear();
							}
						}
						else {
							RegisterMap::iterator it 
								= map.find((instr.*operands[i]).registerName());

							PTXOperand::RegisterType reg = 0;
							if (it == map.end()) {
								reg = (PTXOperand::RegisterType) map.size();
								map.insert(std::make_pair( 
									(instr.*operands[i]).registerName(), reg));
							}
							else {
								reg = it->second;
							}
							(instr.*operands[i]).reg = reg;
							report("  Assigning register " 
								<< (instr.*operands[i]).registerName() 
								<< " to " << reg);
							(instr.*operands[i]).identifier.clear();
						}
					}
				}
			}
		}

		return map;
	}

	void PTXKernel::write(std::ostream& stream) const 
	{
		stream << "/*\n* Ocelot Version : " 
			<< hydrazine::Version().toString() << "\n*/\n";
	
		stream << ".entry " << name;
		if (parameters.size()) {
			stream << "(";
			for( ParameterVector::const_iterator parameter = parameters.begin();
				parameter != parameters.end(); ++parameter )
			{
				if( parameter != parameters.begin() )
				{
					stream << ",\n\t\t" << parameter->toString();
				}
				else
				{
					stream << parameter->toString();
				}
			}
			stream << ")\n";
		}
		stream << "{\n";
		
		for (LocalMap::const_iterator local = locals.begin();
			local != locals.end(); ++local) {
			stream << "\t" << local->second.toString() << "\n";
		}
		
		if (_dfg == NULL) {
			// print register declarations without using DFG
			RegisterTypeMap registerDeclarations = getReferencedRegistersWithoutDFG();

			RegisterTypeMap::iterator regDec = registerDeclarations.begin();
			for (; regDec != registerDeclarations.end(); ++regDec) {
				analysis::DataflowGraph::RegisterId registerId = regDec->first;

				// Don't print it global variables declarations, they are already declared.
				//if (module->globals.count(registerName) == 0) {

					// Don't print shared variables, they will be printed later.
					//if (locals.count(registerName) == 0) {
						ir::PTXOperand::DataType registerType = regDec->second;

						if (registerType == ir::PTXOperand::pred) {
							stream << "\t.reg .pred %p" << registerId << ";\n";
						} else {
							stream << "\t.reg ." << ir::PTXOperand::toString(registerType) << " %r" << registerId << ";\n";
						}
					//}
				//}
			}
		} else {
			RegisterVector regs = getReferencedRegisters();
		
			for( RegisterVector::const_iterator reg = regs.begin();
				reg != regs.end(); ++reg )
			{
				if (reg->type == PTXOperand::pred) {
					stream << "\t.reg .pred %p" << reg->id << ";\n";
				}
				else {
					stream << "\t.reg ." 
						<< PTXOperand::toString( reg->type ) << " " 
						<< "%r" << reg->id << ";\n";
				}
			}
		}
		
		if (_cfg != 0) {
			ControlFlowGraph::BlockPointerVector 
				blocks = _cfg->executable_sequence();
		
			int blockIndex = 1;
			for (ControlFlowGraph::BlockPointerVector::iterator 
				block = blocks.begin(); block != blocks.end(); 
				++block, ++blockIndex) {
				std::string label = (*block)->label;
				std::string comment = (*block)->comment;
				if ((*block)->instructions.size() 
					|| (label != "entry" && label != "exit" && label != "")) {
					if (label == "") {
						std::stringstream ss;
						ss << "$__Block_" << blockIndex;
						label = ss.str();
					}
					stream << label << ":";
					if (comment != "") {
						stream << "\t\t\t\t/* " << comment << " */ ";
					}
					stream << "\n";
				}
				
				for( ControlFlowGraph::InstructionList::iterator 
					instruction = (*block)->instructions.begin(); 
					instruction != (*block)->instructions.end();
					++instruction ) {
					stream << "\t" << (*instruction)->toString() << ";\n";
				}
			}
		}
		stream << "}\n";
	}

	/*! \brief renames all the blocks with canonical names */
	void PTXKernel::canonicalBlockLabels(int kernelID) {

		// visit every block and map the old label to the new label
		std::map<std::string, std::string> labelMap;
		
		for (ControlFlowGraph::iterator 
			block = cfg()->begin(); 
			block != cfg()->end(); ++block) { 

			if (block == cfg()->get_entry_block()) continue;
			if (block == cfg()->get_exit_block()) continue;
			
			std::stringstream ss;
			ss << "$BB_" << kernelID << "_";
			ss.fill('0');
			ss.width(4);
			ss << block->id;
			ss.width(0);
			labelMap[block->label] = ss.str();
			block->comment = block->label;
			block->label = ss.str();
		}
		
		// visit every branch and rewrite the branch target according to the label map

		for (ControlFlowGraph::iterator block = cfg()->begin(); 
			block != cfg()->end(); ++block) {
			for (ControlFlowGraph::InstructionList::iterator 
				instruction = block->instructions.begin(); 
				instruction != block->instructions.end(); ++instruction) {
				PTXInstruction &instr = *static_cast<PTXInstruction*>(
					*instruction);
				if (instr.opcode == ir::PTXInstruction::Bra) {
					instr.d.identifier = labelMap[instr.d.identifier];
				}
			}
		}
	}
}

#endif

