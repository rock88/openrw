#include <script/ScriptMachine.hpp>
#include <script/SCMFile.hpp>
#include <boost/iterator/iterator_concepts.hpp>

#if SCM_DEBUG_INSTRUCTIONS
#include <iostream>
#endif

void ScriptMachine::executeThread(SCMThread &t, int msPassed)
{
	if( t.wakeCounter > 0 ) {
		t.wakeCounter = std::max( t.wakeCounter - msPassed, 0 );
	}
	if( t.wakeCounter > 0 ) return;
	
	while( t.wakeCounter == 0 ) {
		auto opcode = _file->read<SCMOpcode>(t.programCounter);
		auto opcorg = opcode;

		bool isNegatedConditional = ((opcode & SCM_NEGATE_CONDITIONAL_MASK) == SCM_NEGATE_CONDITIONAL_MASK);
		opcode = opcode & ~SCM_NEGATE_CONDITIONAL_MASK;

		auto it = _ops->codes.find(opcode);
		if( it == _ops->codes.end() ) throw IllegalInstruction(opcode, t.programCounter, t.name);
		t.programCounter += sizeof(SCMOpcode);

		SCMMicrocode& code = it->second;

		SCMParams parameters;
		
		bool hasExtraParameters = code.parameters < 0;
		auto requiredParams = std::abs(code.parameters);

		for( int p = 0; p < requiredParams || hasExtraParameters; ++p ) {
			auto type_r = _file->read<SCMByte>(t.programCounter);
			auto type = static_cast<SCMType>(type_r);

			if( type_r > 42 ) {
				// for implicit strings, we need the byte we just read.
				type = TString;
			}
			else {
				t.programCounter += sizeof(SCMByte);
			}

			parameters.push_back(SCMOpcodeParameter { type, { 0 } });
			switch(type) {
			case EndOfArgList:
				hasExtraParameters = false;
				break;
			case TInt8:
				parameters.back().integer = _file->read<std::uint8_t>(t.programCounter);
				t.programCounter += sizeof(SCMByte);
				break;
			case TInt16:
				parameters.back().integer = _file->read<std::int16_t>(t.programCounter);
				t.programCounter += sizeof(SCMByte) * 2;
				break;
			case TGlobal: {
				auto v = _file->read<std::uint16_t>(t.programCounter);
				parameters.back().globalPtr = _globals + v * (SCM_VARIABLE_SIZE/4);
				t.programCounter += sizeof(SCMByte) * 2;
			}
				break;
			case TLocal: {
				auto v = _file->read<std::uint16_t>(t.programCounter);
				parameters.back().globalPtr = t.locals + v * (SCM_VARIABLE_SIZE/4);
				t.programCounter += sizeof(SCMByte) * 2;
			}
				break;
			case TInt32:
				parameters.back().integer = _file->read<std::uint32_t>(t.programCounter);
				t.programCounter += sizeof(SCMByte) * 4;
				break;
			case TString:
				std::copy(_file->data()+t.programCounter, _file->data()+t.programCounter+8,
						  parameters.back().string);
				t.programCounter += sizeof(SCMByte) * 8;
				break;
			case TFloat16:
				parameters.back().real = _file->read<std::int16_t>(t.programCounter) / 16.f;
				t.programCounter += sizeof(SCMByte) * 2;
				break;
			default:
				throw UnknownType(type, t.programCounter, t.name);
				break;
			};
		}

		if(! code.func)
		{
#if SCM_DEBUG_INSTRUCTIONS
			std::cout << std::setw(7) << std::setfill(' ') << t.name <<
			" " << std::dec << std::setw(8) << std::setfill(' ') << t.programCounter <<
			" " << std::hex  << std::setw(4) << std::setfill('0') << opcorg << 
			" " << std::dec;
			for(SCMOpcodeParameter& p : parameters) {
				std::cout << p.type << ":";
				switch(p.type) {
					case TGlobal:
					case TLocal:
						std::cout << *p.globalInteger;
						break;
					case TInt8:
					case TInt16:
					case TInt32:
						std::cout << p.integer;
						break;
					case TFloat16:
						std::cout << p.real;
						break;
					case TString:
						std::cout << p.string;
						break;
				}
				std::cout << " ";
			}
			std::cout << code.name  << " unimplemented"<< std::endl;
#endif
		}
		else
		{
#if SCM_DEBUG_INSTRUCTIONS
			std::string threadfilter;
			if(getenv("SCM_DEBUG_THREAD"))
			{
				threadfilter = getenv("SCM_DEBUG_THREAD");
			}
			if( threadfilter.empty() || threadfilter.find(t.name) != std::string::npos || threadfilter.find(std::to_string(t.baseAddress)) != std::string::npos )
			{
				std::cout << std::setw(7) << std::setfill(' ') << t.name <<
				" " << std::dec << std::setw(8) << std::setfill(' ') << t.programCounter <<
				" " << std::hex  << std::setw(4) << std::setfill('0') << opcorg << 
				" " << std::dec;
				for(SCMOpcodeParameter& p : parameters) {
					std::cout << p.type << ":";
					switch(p.type) {
						case TGlobal:
						case TLocal:
							std::cout << *p.globalInteger << "(" << p.globalInteger << ")";
							break;
						case TInt8:
						case TInt16:
						case TInt32:
							std::cout << p.integer;
							break;
						case TFloat16:
							std::cout << p.real;
							break;
						case TString:
							std::cout << p.string;
							break;
					}
					std::cout << " ";
				}
				std::cout << code.name << std::endl;
			}
#endif
			code.func(this, &t, &parameters);
		}

		if(isNegatedConditional) {
			t.conditionResult = !t.conditionResult;
		}

		// Handle conditional results for IF statements.
		if( t.conditionCount > 0 && opcode != 0x00D6 ) /// @todo add conditional flag to opcodes instead of checking for 0x00D6
		{
			auto cI = --t.conditionCount;
			if ( t.conditionAND )
			{
				if ( t.conditionResult == false ) 
				{
					t.conditionMask = 0;
				}
				else
				{
					// t.conditionMask is already set to 0xFF by the if and opcode.
				}
			}
			else
			{
				t.conditionMask |= (!! t.conditionResult) << cI;
			}
			
			t.conditionResult = (t.conditionMask != 0);
		}
	}

	if( t.wakeCounter == -1 ) {
		t.wakeCounter = 0;
	}
}

#include <iostream>
ScriptMachine::ScriptMachine(GameWorld *world, SCMFile *file, SCMOpcodes *ops)
	: _file(file), _ops(ops), _world(world)
{
	startThread(0);
	auto globals = _file->getGlobalsSize() / 4;
	_globals = new SCMByte[globals * SCM_VARIABLE_SIZE];
	std::cout << globals << " " << SCM_VARIABLE_SIZE << std::endl;
}

ScriptMachine::~ScriptMachine()
{
	delete _file;
	delete _ops;
	delete[] _globals;
}

void ScriptMachine::startThread(SCMThread::pc_t start, bool mission)
{
	SCMThread t;
	for(int i = 0; i < SCM_THREAD_LOCAL_SIZE; ++i) {
		t.locals[i] = 0;
	}
	t.name = "THREAD";
	t.conditionResult = false;
	t.conditionCount = 0;
	t.conditionAND = false;
	t.programCounter = start;
	t.baseAddress = start; /* Indicates where negative jumps should jump from */
	t.wakeCounter = 0;
	t.isMission = mission;
	t.finished = false;
	_activeThreads.push_back(t);
}

SCMByte *ScriptMachine::getGlobals()
{
	return _file->data() + _file->getGlobalSection();
}

void ScriptMachine::execute(float dt)
{
	int ms = dt * 1000.f;
	for(size_t ti = 0; ti < _activeThreads.size(); ++ti) {
		auto thread	= _activeThreads[ti];
		executeThread( thread, ms );

		if( thread.finished ) {
			_activeThreads.erase( _activeThreads.begin() + ti );
		}
		else {
			_activeThreads[ti] = thread;
		}
	}
}
