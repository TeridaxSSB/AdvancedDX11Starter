#include "SimpleShader.h"

// Default error reporting state
bool ISimpleShader::ReportErrors = false;
bool ISimpleShader::ReportWarnings = false;

// To enable error reporting, use either or both 
// of the following lines somewhere in your program, 
// preferably before loading/using any shaders.
// 
// ISimpleShader::ReportErrors = true;
// ISimpleShader::ReportWarnings = true;


///////////////////////////////////////////////////////////////////////////////
// ------ BASE SIMPLE SHADER --------------------------------------------------
///////////////////////////////////////////////////////////////////////////////

// --------------------------------------------------------
// Constructor accepts Direct3D device & context
// --------------------------------------------------------
ISimpleShader::ISimpleShader(Microsoft::WRL::ComPtr<ID3D11Device> device, Microsoft::WRL::ComPtr<ID3D11DeviceContext> context)
{
	// Save the device
	this->device = device;
	this->deviceContext = context;

	// Set up fields
	this->constantBufferCount = 0;
	this->constantBuffers = 0;
	this->shaderValid = false;
}

// --------------------------------------------------------
// Destructor
// --------------------------------------------------------
ISimpleShader::~ISimpleShader()
{
	// Derived class destructors will call this class's CleanUp method
}

// --------------------------------------------------------
// Cleans up the variable table and buffers - Some things will
// be handled by derived classes
// --------------------------------------------------------
void ISimpleShader::CleanUp()
{
	// Handle constant buffers and local data buffers
	for (unsigned int i = 0; i < constantBufferCount; i++)
	{
		delete[] constantBuffers[i].LocalDataBuffer;
	}

	if (constantBuffers)
	{
		delete[] constantBuffers;
		constantBufferCount = 0;
	}

	for (unsigned int i = 0; i < shaderResourceViews.size(); i++)
		delete shaderResourceViews[i];

	for (unsigned int i = 0; i < samplerStates.size(); i++)
		delete samplerStates[i];

	// Clean up tables
	varTable.clear();
	cbTable.clear();
	samplerTable.clear();
	textureTable.clear();
}

// --------------------------------------------------------
// Loads the specified shader and builds the variable table 
// using shader reflection.
//
// shaderFile - A "wide string" specifying the compiled shader to load
// 
// Returns true if shader is loaded properly, false otherwise
// --------------------------------------------------------
bool ISimpleShader::LoadShaderFile(LPCWSTR shaderFile)
{
	// Load the shader to a blob and ensure it worked
	HRESULT hr = D3DReadFileToBlob(shaderFile, shaderBlob.GetAddressOf());
	if (hr != S_OK)
	{
		if (ReportErrors)
		{
			LogError("SimpleShader::LoadShaderFile() - Error loading file '");
			LogW(shaderFile);
			LogError("'. Ensure this file exists and is spelled correctly.\n");
		}

		return false;
	}

	// Create the shader - Calls an overloaded version of this abstract
	// method in the appropriate child class
	shaderValid = CreateShader(shaderBlob);
	if (!shaderValid)
	{
		if (ReportErrors)
		{
			LogError("SimpleShader::LoadShaderFile() - Error creating shader from file '");
			LogW(shaderFile);
			LogError("'. Ensure the type of shader (vertex, pixel, etc.) matches the SimpleShader type (SimpleVertexShader, SimplePixelShader, etc.) you're using.\n");
		}

		return false;
	}

	// Set up shader reflection to get information about
	// this shader and its variables,  buffers, etc.
	Microsoft::WRL::ComPtr<ID3D11ShaderReflection> refl;
	D3DReflect(
		shaderBlob->GetBufferPointer(),
		shaderBlob->GetBufferSize(),
		IID_ID3D11ShaderReflection,
		(void**)refl.GetAddressOf());

	// Get the description of the shader
	D3D11_SHADER_DESC shaderDesc;
	refl->GetDesc(&shaderDesc);

	// Create resource arrays
	constantBufferCount = shaderDesc.ConstantBuffers;
	constantBuffers = new SimpleConstantBuffer[constantBufferCount];

	// Handle bound resources (like shaders and samplers)
	unsigned int resourceCount = shaderDesc.BoundResources;
	for (unsigned int r = 0; r < resourceCount; r++)
	{
		// Get this resource's description
		D3D11_SHADER_INPUT_BIND_DESC resourceDesc;
		refl->GetResourceBindingDesc(r, &resourceDesc);

		// Check the type
		switch (resourceDesc.Type)
		{
		case D3D_SIT_STRUCTURED: // Treat structured buffers as texture resources
		case D3D_SIT_TEXTURE: // A texture resource
		{
			// Create the SRV wrapper
			SimpleSRV* srv = new SimpleSRV();
			srv->BindIndex = resourceDesc.BindPoint;				// Shader bind point
			srv->Index = (unsigned int)shaderResourceViews.size();	// Raw index

			textureTable.insert(std::pair<std::string, SimpleSRV*>(resourceDesc.Name, srv));
			shaderResourceViews.push_back(srv);
		}
		break;

		case D3D_SIT_SAMPLER: // A sampler resource
		{
			// Create the sampler wrapper
			SimpleSampler* samp = new SimpleSampler();
			samp->BindIndex = resourceDesc.BindPoint;			// Shader bind point
			samp->Index = (unsigned int)samplerStates.size();	// Raw index

			samplerTable.insert(std::pair<std::string, SimpleSampler*>(resourceDesc.Name, samp));
			samplerStates.push_back(samp);
		}
		break;
		}
	}

	// Loop through all constant buffers
	for (unsigned int b = 0; b < constantBufferCount; b++)
	{
		// Get this buffer
		ID3D11ShaderReflectionConstantBuffer* cb =
			refl->GetConstantBufferByIndex(b);

		// Get the description of this buffer
		D3D11_SHADER_BUFFER_DESC bufferDesc;
		cb->GetDesc(&bufferDesc);

		// Save the type, which we reference when setting these buffers
		constantBuffers[b].Type = bufferDesc.Type;

		// Get the description of the resource binding, so
		// we know exactly how it's bound in the shader
		D3D11_SHADER_INPUT_BIND_DESC bindDesc;
		refl->GetResourceBindingDescByName(bufferDesc.Name, &bindDesc);

		// Set up the buffer and put its pointer in the table
		constantBuffers[b].BindIndex = bindDesc.BindPoint;
		constantBuffers[b].Name = bufferDesc.Name;
		cbTable.insert(std::pair<std::string, SimpleConstantBuffer*>(bufferDesc.Name, &constantBuffers[b]));

		// Create this constant buffer
		D3D11_BUFFER_DESC newBuffDesc = {};
		newBuffDesc.Usage = D3D11_USAGE_DEFAULT;
		newBuffDesc.ByteWidth = bufferDesc.Size;
		newBuffDesc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
		newBuffDesc.CPUAccessFlags = 0;
		newBuffDesc.MiscFlags = 0;
		newBuffDesc.StructureByteStride = 0;
		device->CreateBuffer(&newBuffDesc, 0, constantBuffers[b].ConstantBuffer.GetAddressOf());

		// Set up the data buffer for this constant buffer
		constantBuffers[b].Size = bufferDesc.Size;
		constantBuffers[b].LocalDataBuffer = new unsigned char[bufferDesc.Size];
		ZeroMemory(constantBuffers[b].LocalDataBuffer, bufferDesc.Size);

		// Loop through all variables in this buffer
		for (unsigned int v = 0; v < bufferDesc.Variables; v++)
		{
			// Get this variable
			ID3D11ShaderReflectionVariable* var =
				cb->GetVariableByIndex(v);

			// Get the description of the variable and its type
			D3D11_SHADER_VARIABLE_DESC varDesc;
			var->GetDesc(&varDesc);

			// Create the variable struct
			SimpleShaderVariable varStruct = {};
			varStruct.ConstantBufferIndex = b;
			varStruct.ByteOffset = varDesc.StartOffset;
			varStruct.Size = varDesc.Size;

			// Get a string version
			std::string varName(varDesc.Name);

			// Add this variable to the table and the constant buffer
			varTable.insert(std::pair<std::string, SimpleShaderVariable>(varName, varStruct));
			constantBuffers[b].Variables.push_back(varStruct);
		}
	}

	// All set
	return true;
}

// --------------------------------------------------------
// Helper for looking up a variable by name and also
// verifying that it is the requested size
// 
// name - the name of the variable to look for
// size - the size of the variable (for verification), or -1 to bypass
// --------------------------------------------------------
SimpleShaderVariable* ISimpleShader::FindVariable(std::string name, int size)
{
	// Look for the key
	std::unordered_map<std::string, SimpleShaderVariable>::iterator result =
		varTable.find(name);

	// Did we find the key?
	if (result == varTable.end())
		return 0;

	// Grab the result from the iterator
	SimpleShaderVariable* var = &(result->second);

	// Is the data size correct ?
	if (size > 0 && var->Size != size)
		return 0;

	// Success
	return var;
}

// --------------------------------------------------------
// Helper for looking up a constant buffer by name
// --------------------------------------------------------
SimpleConstantBuffer* ISimpleShader::FindConstantBuffer(std::string name)
{
	// Look for the key
	std::unordered_map<std::string, SimpleConstantBuffer*>::iterator result =
		cbTable.find(name);

	// Did we find the key?
	if (result == cbTable.end())
		return 0;

	// Success
	return result->second;
}

// --------------------------------------------------------
// Prints the specified message to the console with the 
// given color and Visual Studio's output window
// --------------------------------------------------------
void ISimpleShader::Log(std::string message, WORD color)
{
	// Swap console color
	HANDLE hConsole = GetStdHandle(STD_OUTPUT_HANDLE);
	SetConsoleTextAttribute(hConsole, color);

	printf_s(message.c_str());
	OutputDebugString(message.c_str());

	// Swap back
	SetConsoleTextAttribute(hConsole, FOREGROUND_RED | FOREGROUND_GREEN | FOREGROUND_BLUE);
}

// --------------------------------------------------------
// Prints the specified message, as a wide string, to the 
// console with the given color and Visual Studio's output window
// --------------------------------------------------------
void ISimpleShader::LogW(std::wstring message, WORD color)
{
	// Swap console color
	HANDLE hConsole = GetStdHandle(STD_OUTPUT_HANDLE);
	SetConsoleTextAttribute(hConsole, color);

	wprintf_s(message.c_str());
	OutputDebugStringW(message.c_str());

	// Swap back
	SetConsoleTextAttribute(hConsole, FOREGROUND_RED | FOREGROUND_GREEN | FOREGROUND_BLUE);
}


// Helpers for pritning errors and warnings in specific colors using regular and wide character strings
void ISimpleShader::Log(std::string message) { Log(message, FOREGROUND_RED | FOREGROUND_GREEN | FOREGROUND_BLUE | FOREGROUND_INTENSITY); }
void ISimpleShader::LogW(std::wstring message) { LogW(message, FOREGROUND_RED | FOREGROUND_GREEN | FOREGROUND_BLUE | FOREGROUND_INTENSITY); }
void ISimpleShader::LogError(std::string message) { Log(message, FOREGROUND_RED | FOREGROUND_INTENSITY); }
void ISimpleShader::LogErrorW(std::wstring message) { LogW(message, FOREGROUND_RED | FOREGROUND_INTENSITY); }
void ISimpleShader::LogWarning(std::string message) { Log(message, FOREGROUND_RED | FOREGROUND_GREEN | FOREGROUND_INTENSITY); }
void ISimpleShader::LogWarningW(std::wstring message) { LogW(message, FOREGROUND_RED | FOREGROUND_GREEN | FOREGROUND_INTENSITY); }


// --------------------------------------------------------
// Sets the shader and associated constant buffers in Direct3D
// --------------------------------------------------------
void ISimpleShader::SetShader()
{
	// Ensure the shader is valid
	if (!shaderValid) return;

	// Set the shader and any relevant constant buffers, which
	// is an overloaded method in a subclass
	SetShaderAndCBs();
}

// --------------------------------------------------------
// Copies the relevant data to the all of this 
// shader's constant buffers.  To just copy one
// buffer, use CopyBufferData()
// --------------------------------------------------------
void ISimpleShader::CopyAllBufferData()
{
	// Ensure the shader is valid
	if (!shaderValid) return;

	// Loop through the constant buffers and copy all data
	for (unsigned int i = 0; i < constantBufferCount; i++)
	{
		// Copy the entire local data buffer
		deviceContext->UpdateSubresource(
			constantBuffers[i].ConstantBuffer.Get(), 0, 0,
			constantBuffers[i].LocalDataBuffer, 0, 0);
	}
}

// --------------------------------------------------------
// Copies local data to the shader's specified constant buffer
//
// index - The index of the buffer to copy.
//         Useful for updating more frequently-changing
//         variables without having to re-copy all buffers.
//  
// NOTE: The "index" of the buffer might NOT be the same
//       as its register, especially if you have buffers
//       bound to non-sequential registers!
// --------------------------------------------------------
void ISimpleShader::CopyBufferData(unsigned int index)
{
	// Ensure the shader is valid
	if (!shaderValid) return;

	// Validate the index
	if (index >= this->constantBufferCount)
		return;

	// Check for the buffer
	SimpleConstantBuffer* cb = &this->constantBuffers[index];
	if (!cb) return;

	// Copy the data and get out
	deviceContext->UpdateSubresource(
		cb->ConstantBuffer.Get(), 0, 0,
		cb->LocalDataBuffer, 0, 0);
}

// --------------------------------------------------------
// Copies local data to the shader's specified constant buffer
//
// bufferName - Specifies the name of the buffer to copy.
//              Useful for updating more frequently-changing
//              variables without having to re-copy all buffers.
// --------------------------------------------------------
void ISimpleShader::CopyBufferData(std::string bufferName)
{
	// Ensure the shader is valid
	if (!shaderValid) return;

	// Check for the buffer
	SimpleConstantBuffer* cb = this->FindConstantBuffer(bufferName);
	if (!cb) return;

	// Copy the data and get out
	deviceContext->UpdateSubresource(
		cb->ConstantBuffer.Get(), 0, 0,
		cb->LocalDataBuffer, 0, 0);
}


// --------------------------------------------------------
// Sets a variable by name with arbitrary data of the specified size
//
// name - The name of the shader variable
// data - The data to set in the buffer
// size - The size of the data (this must be less than or equal to the variable's size)
//
// Returns true if data is copied, false if variable doesn't exist
// --------------------------------------------------------
bool ISimpleShader::SetData(std::string name, const void* data, unsigned int size)
{
	// Look for the variable and verify
	SimpleShaderVariable* var = FindVariable(name, -1);
	if (var == 0)
	{
		if (ReportWarnings)
		{
			LogWarning("SimpleShader::SetData() - Shader variable '");
			Log(name);
			LogWarning("' not found. Ensure the name is spelled correctly and that it exists in a constant buffer in the shader.\n");
		}
		return false;
	}

	// Ensure we're not trying to copy more data than the variable can hold
	// Note: We can copy less data, in the case of a subset of an array
	if (size > var->Size)
	{
		if (ReportWarnings)
		{
			LogWarning("SimpleShader::SetData() - Shader variable '");
			Log(name);
			LogWarning("' is smaller than the size of the data being set. Ensure the variable is large enough for the specified data.\n");
		}
		return false;
	}

	// Set the data in the local data buffer
	memcpy(
		constantBuffers[var->ConstantBufferIndex].LocalDataBuffer + var->ByteOffset,
		data,
		size);

	// Success
	return true;
}

// --------------------------------------------------------
// Sets INTEGER data
// --------------------------------------------------------
bool ISimpleShader::SetInt(std::string name, int data)
{
	return this->SetData(name, (void*)(&data), sizeof(int));
}

// --------------------------------------------------------
// Sets a FLOAT variable by name in the local data buffer
// --------------------------------------------------------
bool ISimpleShader::SetFloat(std::string name, float data)
{
	return this->SetData(name, (void*)(&data), sizeof(float));
}

// --------------------------------------------------------
// Sets a FLOAT2 variable by name in the local data buffer
// --------------------------------------------------------
bool ISimpleShader::SetFloat2(std::string name, const float data[2])
{
	return this->SetData(name, (void*)data, sizeof(float) * 2);
}

// --------------------------------------------------------
// Sets a FLOAT2 variable by name in the local data buffer
// --------------------------------------------------------
bool ISimpleShader::SetFloat2(std::string name, const DirectX::XMFLOAT2 data)
{
	return this->SetData(name, &data, sizeof(float) * 2);
}

// --------------------------------------------------------
// Sets a FLOAT3 variable by name in the local data buffer
// --------------------------------------------------------
bool ISimpleShader::SetFloat3(std::string name, const float data[3])
{
	return this->SetData(name, (void*)data, sizeof(float) * 3);
}

// --------------------------------------------------------
// Sets a FLOAT3 variable by name in the local data buffer
// --------------------------------------------------------
bool ISimpleShader::SetFloat3(std::string name, const DirectX::XMFLOAT3 data)
{
	return this->SetData(name, &data, sizeof(float) * 3);
}

// --------------------------------------------------------
// Sets a FLOAT4 variable by name in the local data buffer
// --------------------------------------------------------
bool ISimpleShader::SetFloat4(std::string name, const float data[4])
{
	return this->SetData(name, (void*)data, sizeof(float) * 4);
}

// --------------------------------------------------------
// Sets a FLOAT4 variable by name in the local data buffer
// --------------------------------------------------------
bool ISimpleShader::SetFloat4(std::string name, const DirectX::XMFLOAT4 data)
{
	return this->SetData(name, &data, sizeof(float) * 4);
}

// --------------------------------------------------------
// Sets a MATRIX (4x4) variable by name in the local data buffer
// --------------------------------------------------------
bool ISimpleShader::SetMatrix4x4(std::string name, const float data[16])
{
	return this->SetData(name, (void*)data, sizeof(float) * 16);
}

// --------------------------------------------------------
// Sets a MATRIX (4x4) variable by name in the local data buffer
// --------------------------------------------------------
bool ISimpleShader::SetMatrix4x4(std::string name, const DirectX::XMFLOAT4X4 data)
{
	return this->SetData(name, &data, sizeof(float) * 16);
}

// --------------------------------------------------------
// Determines if the shader contains the specified
// variable within one of its constant buffers
// --------------------------------------------------------
bool ISimpleShader::HasVariable(std::string name)
{
	return FindVariable(name, -1) != 0;
}

// --------------------------------------------------------
// Determines if the shader contains the specified SRV
// --------------------------------------------------------
bool ISimpleShader::HasShaderResourceView(std::string name)
{
	return GetShaderResourceViewInfo(name) != 0;
}

// --------------------------------------------------------
// Determines if the shader contains the specified sampler
// --------------------------------------------------------
bool ISimpleShader::HasSamplerState(std::string name)
{
	return GetSamplerInfo(name) != 0;
}

// --------------------------------------------------------
// Gets info about a shader variable, if it exists
// --------------------------------------------------------
const SimpleShaderVariable* ISimpleShader::GetVariableInfo(std::string name)
{
	return FindVariable(name, -1);
}

// --------------------------------------------------------
// Gets info about an SRV in the shader (or null)
//
// name - the name of the SRV
// --------------------------------------------------------
const SimpleSRV* ISimpleShader::GetShaderResourceViewInfo(std::string name)
{
	// Look for the key
	std::unordered_map<std::string, SimpleSRV*>::iterator result =
		textureTable.find(name);

	// Did we find the key?
	if (result == textureTable.end())
		return 0;

	// Success
	return result->second;
}


// --------------------------------------------------------
// Gets info about an SRV in the shader (or null)
//
// index - the index of the SRV
// --------------------------------------------------------
const SimpleSRV* ISimpleShader::GetShaderResourceViewInfo(unsigned int index)
{
	// Valid index?
	if (index >= shaderResourceViews.size()) return 0;

	// Grab the bind index
	return shaderResourceViews[index];
}


// --------------------------------------------------------
// Gets info about a sampler in the shader (or null)
// 
// name - the name of the sampler
// --------------------------------------------------------
const SimpleSampler* ISimpleShader::GetSamplerInfo(std::string name)
{
	// Look for the key
	std::unordered_map<std::string, SimpleSampler*>::iterator result =
		samplerTable.find(name);

	// Did we find the key?
	if (result == samplerTable.end())
		return 0;

	// Success
	return result->second;
}

// --------------------------------------------------------
// Gets info about a sampler in the shader (or null)
// 
// index - the index of the sampler
// --------------------------------------------------------
const SimpleSampler* ISimpleShader::GetSamplerInfo(unsigned int index)
{
	// Valid index?
	if (index >= samplerStates.size()) return 0;

	// Grab the bind index
	return samplerStates[index];
}


// --------------------------------------------------------
// Gets the number of constant buffers in this shader
// --------------------------------------------------------
unsigned int ISimpleShader::GetBufferCount() { return constantBufferCount; }



// --------------------------------------------------------
// Gets the size of a particular constant buffer, or -1
// --------------------------------------------------------
unsigned int ISimpleShader::GetBufferSize(unsigned int index)
{
	// Valid index?
	if (index >= constantBufferCount)
		return -1;

	// Grab the size
	return constantBuffers[index].Size;
}

// --------------------------------------------------------
// Gets info about a particular constant buffer 
// by name, if it exists
// --------------------------------------------------------
const SimpleConstantBuffer* ISimpleShader::GetBufferInfo(std::string name)
{
	return FindConstantBuffer(name);
}

// --------------------------------------------------------
// Gets info about a particular constant buffer 
//
// index - the index of the constant buffer
// --------------------------------------------------------
const SimpleConstantBuffer* ISimpleShader::GetBufferInfo(unsigned int index)
{
	// Check for valid index
	if (index >= constantBufferCount) return 0;

	// Return the specific buffer
	return &constantBuffers[index];
}





///////////////////////////////////////////////////////////////////////////////
// ------ SIMPLE VERTEX SHADER ------------------------------------------------
///////////////////////////////////////////////////////////////////////////////

// --------------------------------------------------------
// Constructor just calls the base
// --------------------------------------------------------
SimpleVertexShader::SimpleVertexShader(Microsoft::WRL::ComPtr<ID3D11Device> device, Microsoft::WRL::ComPtr<ID3D11DeviceContext> context, LPCWSTR shaderFile)
	: ISimpleShader(device, context)
{
	// Ensure we set to zero to successfully trigger
	// the Input Layout creation during LoadShaderFile()
	this->perInstanceCompatible = false;

	// Load the actual compiled shader file
	this->LoadShaderFile(shaderFile);
}

// --------------------------------------------------------
// Constructor overload which takes a custom input layout
//
// Passing in a valid input layout will stop LoadShaderFile()
// from creating an input layout from shader reflection
// --------------------------------------------------------
SimpleVertexShader::SimpleVertexShader(Microsoft::WRL::ComPtr<ID3D11Device> device, Microsoft::WRL::ComPtr<ID3D11DeviceContext> context, LPCWSTR shaderFile, Microsoft::WRL::ComPtr<ID3D11InputLayout> inputLayout, bool perInstanceCompatible)
	: ISimpleShader(device, context)
{
	// Save the custom input layout
	this->inputLayout = inputLayout;

	// Unable to determine from an input layout, require user to tell us
	this->perInstanceCompatible = perInstanceCompatible;

	// Load the actual compiled shader file
	this->LoadShaderFile(shaderFile);
}

// --------------------------------------------------------
// Destructor - Clean up actual shader (base will be called automatically)
// --------------------------------------------------------
SimpleVertexShader::~SimpleVertexShader()
{
	CleanUp();
}

// --------------------------------------------------------
// Handles cleaning up shader and base class clean up
// --------------------------------------------------------
void SimpleVertexShader::CleanUp()
{
	ISimpleShader::CleanUp();
}

// --------------------------------------------------------
// Creates the  Direct3D vertex shader
//
// shaderBlob - The shader's compiled code
//
// Returns true if shader is created correctly, false otherwise
// --------------------------------------------------------
bool SimpleVertexShader::CreateShader(Microsoft::WRL::ComPtr<ID3DBlob> shaderBlob)
{
	// Clean up first, in the event this method is
	// called more than once on the same object
	this->CleanUp();

	// Create the shader from the blob
	HRESULT result = device->CreateVertexShader(
		shaderBlob->GetBufferPointer(),
		shaderBlob->GetBufferSize(),
		0,
		shader.GetAddressOf());

	// Did the creation work?
	if (result != S_OK)
		return false;

	// Do we already have an input layout?
	// (This would come from one of the constructor overloads)
	if (inputLayout)
		return true;

	// Vertex shader was created successfully, so we now use the
	// shader code to re-reflect and create an input layout that 
	// matches what the vertex shader expects.  Code adapted from:
	// https://takinginitiative.wordpress.com/2011/12/11/directx-1011-basic-shader-reflection-automatic-input-layout-creation/

	// Reflect shader info
	Microsoft::WRL::ComPtr<ID3D11ShaderReflection> refl;
	D3DReflect(
		shaderBlob->GetBufferPointer(),
		shaderBlob->GetBufferSize(),
		IID_ID3D11ShaderReflection,
		(void**)refl.GetAddressOf());

	// Get shader info
	D3D11_SHADER_DESC shaderDesc;
	refl->GetDesc(&shaderDesc);

	// Read input layout description from shader info
	std::vector<D3D11_INPUT_ELEMENT_DESC> inputLayoutDesc;
	for (unsigned int i = 0; i < shaderDesc.InputParameters; i++)
	{
		D3D11_SIGNATURE_PARAMETER_DESC paramDesc;
		refl->GetInputParameterDesc(i, &paramDesc);

		// Check the semantic name for "_PER_INSTANCE"
		std::string perInstanceStr = "_PER_INSTANCE";
		std::string sem = paramDesc.SemanticName;
		int lenDiff = (int)sem.size() - (int)perInstanceStr.size();
		bool isPerInstance =
			lenDiff >= 0 &&
			sem.compare(lenDiff, perInstanceStr.size(), perInstanceStr) == 0;

		// Fill out input element desc
		D3D11_INPUT_ELEMENT_DESC elementDesc = {};
		elementDesc.SemanticName = paramDesc.SemanticName;
		elementDesc.SemanticIndex = paramDesc.SemanticIndex;
		elementDesc.InputSlot = 0;
		elementDesc.AlignedByteOffset = D3D11_APPEND_ALIGNED_ELEMENT;
		elementDesc.InputSlotClass = D3D11_INPUT_PER_VERTEX_DATA;
		elementDesc.InstanceDataStepRate = 0;

		// Replace anything affected by "per instance" data
		if (isPerInstance)
		{
			elementDesc.InputSlot = 1; // Assume per instance data comes from another input slot!
			elementDesc.InputSlotClass = D3D11_INPUT_PER_INSTANCE_DATA;
			elementDesc.InstanceDataStepRate = 1;

			perInstanceCompatible = true;
		}

		// Determine DXGI format
		if (paramDesc.Mask == 1)
		{
			if (paramDesc.ComponentType == D3D_REGISTER_COMPONENT_UINT32) elementDesc.Format = DXGI_FORMAT_R32_UINT;
			else if (paramDesc.ComponentType == D3D_REGISTER_COMPONENT_SINT32) elementDesc.Format = DXGI_FORMAT_R32_SINT;
			else if (paramDesc.ComponentType == D3D_REGISTER_COMPONENT_FLOAT32) elementDesc.Format = DXGI_FORMAT_R32_FLOAT;
		}
		else if (paramDesc.Mask <= 3)
		{
			if (paramDesc.ComponentType == D3D_REGISTER_COMPONENT_UINT32) elementDesc.Format = DXGI_FORMAT_R32G32_UINT;
			else if (paramDesc.ComponentType == D3D_REGISTER_COMPONENT_SINT32) elementDesc.Format = DXGI_FORMAT_R32G32_SINT;
			else if (paramDesc.ComponentType == D3D_REGISTER_COMPONENT_FLOAT32) elementDesc.Format = DXGI_FORMAT_R32G32_FLOAT;
		}
		else if (paramDesc.Mask <= 7)
		{
			if (paramDesc.ComponentType == D3D_REGISTER_COMPONENT_UINT32) elementDesc.Format = DXGI_FORMAT_R32G32B32_UINT;
			else if (paramDesc.ComponentType == D3D_REGISTER_COMPONENT_SINT32) elementDesc.Format = DXGI_FORMAT_R32G32B32_SINT;
			else if (paramDesc.ComponentType == D3D_REGISTER_COMPONENT_FLOAT32) elementDesc.Format = DXGI_FORMAT_R32G32B32_FLOAT;
		}
		else if (paramDesc.Mask <= 15)
		{
			if (paramDesc.ComponentType == D3D_REGISTER_COMPONENT_UINT32) elementDesc.Format = DXGI_FORMAT_R32G32B32A32_UINT;
			else if (paramDesc.ComponentType == D3D_REGISTER_COMPONENT_SINT32) elementDesc.Format = DXGI_FORMAT_R32G32B32A32_SINT;
			else if (paramDesc.ComponentType == D3D_REGISTER_COMPONENT_FLOAT32) elementDesc.Format = DXGI_FORMAT_R32G32B32A32_FLOAT;
		}

		// Save element desc
		inputLayoutDesc.push_back(elementDesc);
	}

	// Try to create Input Layout
	HRESULT hr = device->CreateInputLayout(
		&inputLayoutDesc[0],
		(unsigned int)inputLayoutDesc.size(),
		shaderBlob->GetBufferPointer(),
		shaderBlob->GetBufferSize(),
		inputLayout.GetAddressOf());

	// All done, clean up
	return true;
}

// --------------------------------------------------------
// Sets the vertex shader, input layout and constant buffers
// for future  Direct3D drawing
// --------------------------------------------------------
void SimpleVertexShader::SetShaderAndCBs()
{
	// Is shader valid?
	if (!shaderValid) return;

	// Set the shader and input layout
	deviceContext->IASetInputLayout(inputLayout.Get());
	deviceContext->VSSetShader(shader.Get(), 0, 0);

	// Set the constant buffers
	for (unsigned int i = 0; i < constantBufferCount; i++)
	{
		// Skip "buffers" that aren't true constant buffers
		if (constantBuffers[i].Type != D3D11_CT_CBUFFER)
			continue;

		// This is a real constant buffer, so set it
		deviceContext->VSSetConstantBuffers(
			constantBuffers[i].BindIndex,
			1,
			constantBuffers[i].ConstantBuffer.GetAddressOf());
	}
}

// --------------------------------------------------------
// Sets a shader resource view in the vertex shader stage
//
// name - The name of the texture resource in the shader
// srv - The shader resource view of the texture in GPU memory
//
// Returns true if a texture of the given name was found, false otherwise
// --------------------------------------------------------
bool SimpleVertexShader::SetShaderResourceView(std::string name, Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> srv)
{
	// Look for the variable and verify
	const SimpleSRV* srvInfo = GetShaderResourceViewInfo(name);
	if (srvInfo == 0)
	{
		if (ReportWarnings)
		{
			LogWarning("SimpleVertexShader::SetShaderResourceView() - SRV named '");
			Log(name);
			LogWarning("' was not found in the shader. Ensure the name is spelled correctly and that it exists in the shader.\n");
		}
		return false;
	}

	// Set the shader resource view
	deviceContext->VSSetShaderResources(srvInfo->BindIndex, 1, srv.GetAddressOf());

	// Success
	return true;
}

// --------------------------------------------------------
// Sets a sampler state in the vertex shader stage
//
// name - The name of the sampler state in the shader
// samplerState - The sampler state in GPU memory
//
// Returns true if a sampler of the given name was found, false otherwise
// --------------------------------------------------------
bool SimpleVertexShader::SetSamplerState(std::string name, Microsoft::WRL::ComPtr<ID3D11SamplerState> samplerState)
{
	// Look for the variable and verify
	const SimpleSampler* sampInfo = GetSamplerInfo(name);
	if (sampInfo == 0)
	{
		if (ReportWarnings)
		{
			LogWarning("SimpleVertexShader::SetSamplerState() - Sampler named '");
			Log(name);
			LogWarning("' was not found in the shader. Ensure the name is spelled correctly and that it exists in the shader.\n");
		}
		return false;
	}

	// Set the shader resource view
	deviceContext->VSSetSamplers(sampInfo->BindIndex, 1, samplerState.GetAddressOf());

	// Success
	return true;
}


///////////////////////////////////////////////////////////////////////////////
// ------ SIMPLE PIXEL SHADER -------------------------------------------------
///////////////////////////////////////////////////////////////////////////////

// --------------------------------------------------------
// Constructor just calls the base
// --------------------------------------------------------
SimplePixelShader::SimplePixelShader(Microsoft::WRL::ComPtr<ID3D11Device> device, Microsoft::WRL::ComPtr<ID3D11DeviceContext> context, LPCWSTR shaderFile)
	: ISimpleShader(device, context)
{
	// Load the actual compiled shader file
	this->LoadShaderFile(shaderFile);
}

// --------------------------------------------------------
// Destructor - Clean up actual shader (base will be called automatically)
// --------------------------------------------------------
SimplePixelShader::~SimplePixelShader()
{
	CleanUp();
}

// --------------------------------------------------------
// Handles cleaning up shader and base class clean up
// --------------------------------------------------------
void SimplePixelShader::CleanUp()
{
	ISimpleShader::CleanUp();
}

// --------------------------------------------------------
// Creates the  Direct3D pixel shader
//
// shaderBlob - The shader's compiled code
//
// Returns true if shader is created correctly, false otherwise
// --------------------------------------------------------
bool SimplePixelShader::CreateShader(Microsoft::WRL::ComPtr<ID3DBlob> shaderBlob)
{
	// Clean up first, in the event this method is
	// called more than once on the same object
	this->CleanUp();

	// Create the shader from the blob
	HRESULT result = device->CreatePixelShader(
		shaderBlob->GetBufferPointer(),
		shaderBlob->GetBufferSize(),
		0,
		shader.GetAddressOf());

	// Check the result
	return (result == S_OK);
}

// --------------------------------------------------------
// Sets the pixel shader and constant buffers for
// future  Direct3D drawing
// --------------------------------------------------------
void SimplePixelShader::SetShaderAndCBs()
{
	// Is shader valid?
	if (!shaderValid) return;

	// Set the shader
	deviceContext->PSSetShader(shader.Get(), 0, 0);

	// Set the constant buffers
	for (unsigned int i = 0; i < constantBufferCount; i++)
	{
		// Skip "buffers" that aren't true constant buffers
		if (constantBuffers[i].Type != D3D11_CT_CBUFFER)
			continue;

		// This is a real constant buffer, so set it
		deviceContext->PSSetConstantBuffers(
			constantBuffers[i].BindIndex,
			1,
			constantBuffers[i].ConstantBuffer.GetAddressOf());
	}
}

// --------------------------------------------------------
// Sets a shader resource view in the pixel shader stage
//
// name - The name of the texture resource in the shader
// srv - The shader resource view of the texture in GPU memory
//
// Returns true if a texture of the given name was found, false otherwise
// --------------------------------------------------------
bool SimplePixelShader::SetShaderResourceView(std::string name, Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> srv)
{
	// Look for the variable and verify
	const SimpleSRV* srvInfo = GetShaderResourceViewInfo(name);
	if (srvInfo == 0)
	{
		if (ReportWarnings)
		{
			LogWarning("SimplePixelShader::SetShaderResourceView() - SRV named '");
			Log(name);
			LogWarning("' was not found in the shader. Ensure the name is spelled correctly and that it exists in the shader.\n");
		}
		return false;
	}

	// Set the shader resource view
	deviceContext->PSSetShaderResources(srvInfo->BindIndex, 1, srv.GetAddressOf());

	// Success
	return true;
}

// --------------------------------------------------------
// Sets a sampler state in the pixel shader stage
//
// name - The name of the sampler state in the shader
// samplerState - The sampler state in GPU memory
//
// Returns true if a sampler of the given name was found, false otherwise
// --------------------------------------------------------
bool SimplePixelShader::SetSamplerState(std::string name, Microsoft::WRL::ComPtr<ID3D11SamplerState> samplerState)
{
	// Look for the variable and verify
	const SimpleSampler* sampInfo = GetSamplerInfo(name);
	if (sampInfo == 0)
	{
		if (ReportWarnings)
		{
			LogWarning("SimplePixelShader::SetSamplerState() - Sampler named '");
			Log(name);
			LogWarning("' was not found in the shader. Ensure the name is spelled correctly and that it exists in the shader.\n");
		}
		return false;
	}

	// Set the shader resource view
	deviceContext->PSSetSamplers(sampInfo->BindIndex, 1, samplerState.GetAddressOf());

	// Success
	return true;
}




///////////////////////////////////////////////////////////////////////////////
// ------ SIMPLE DOMAIN SHADER ------------------------------------------------
///////////////////////////////////////////////////////////////////////////////

// --------------------------------------------------------
// Constructor just calls the base
// --------------------------------------------------------
SimpleDomainShader::SimpleDomainShader(Microsoft::WRL::ComPtr<ID3D11Device> device, Microsoft::WRL::ComPtr<ID3D11DeviceContext> context, LPCWSTR shaderFile)
	: ISimpleShader(device, context)
{
	// Load the actual compiled shader file
	this->LoadShaderFile(shaderFile);
}

// --------------------------------------------------------
// Destructor - Clean up actual shader (base will be called automatically)
// --------------------------------------------------------
SimpleDomainShader::~SimpleDomainShader()
{
	CleanUp();
}

// --------------------------------------------------------
// Handles cleaning up shader and base class clean up
// --------------------------------------------------------
void SimpleDomainShader::CleanUp()
{
	ISimpleShader::CleanUp();
}

// --------------------------------------------------------
// Creates the  Direct3D domain shader
//
// shaderBlob - The shader's compiled code
//
// Returns true if shader is created correctly, false otherwise
// --------------------------------------------------------
bool SimpleDomainShader::CreateShader(Microsoft::WRL::ComPtr<ID3DBlob> shaderBlob)
{
	// Clean up first, in the event this method is
	// called more than once on the same object
	this->CleanUp();

	// Create the shader from the blob
	HRESULT result = device->CreateDomainShader(
		shaderBlob->GetBufferPointer(),
		shaderBlob->GetBufferSize(),
		0,
		shader.GetAddressOf());

	// Check the result
	return (result == S_OK);
}

// --------------------------------------------------------
// Sets the domain shader and constant buffers for
// future  Direct3D drawing
// --------------------------------------------------------
void SimpleDomainShader::SetShaderAndCBs()
{
	// Is shader valid?
	if (!shaderValid) return;

	// Set the shader
	deviceContext->DSSetShader(shader.Get(), 0, 0);

	// Set the constant buffers
	for (unsigned int i = 0; i < constantBufferCount; i++)
	{
		// Skip "buffers" that aren't true constant buffers
		if (constantBuffers[i].Type != D3D11_CT_CBUFFER)
			continue;

		// This is a real constant buffer, so set it
		deviceContext->DSSetConstantBuffers(
			constantBuffers[i].BindIndex,
			1,
			constantBuffers[i].ConstantBuffer.GetAddressOf());
	}
}

// --------------------------------------------------------
// Sets a shader resource view in the domain shader stage
//
// name - The name of the texture resource in the shader
// srv - The shader resource view of the texture in GPU memory
//
// Returns true if a texture of the given name was found, false otherwise
// --------------------------------------------------------
bool SimpleDomainShader::SetShaderResourceView(std::string name, Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> srv)
{
	// Look for the variable and verify
	const SimpleSRV* srvInfo = GetShaderResourceViewInfo(name);
	if (srvInfo == 0)
	{
		if (ReportWarnings)
		{
			LogWarning("SimpleDomainShader::SetShaderResourceView() - SRV named '");
			Log(name);
			LogWarning("' was not found in the shader. Ensure the name is spelled correctly and that it exists in the shader.\n");
		}
		return false;
	}

	// Set the shader resource view
	deviceContext->DSSetShaderResources(srvInfo->BindIndex, 1, srv.GetAddressOf());

	// Success
	return true;
}

// --------------------------------------------------------
// Sets a sampler state in the domain shader stage
//
// name - The name of the sampler state in the shader
// samplerState - The sampler state in GPU memory
//
// Returns true if a sampler of the given name was found, false otherwise
// --------------------------------------------------------
bool SimpleDomainShader::SetSamplerState(std::string name, Microsoft::WRL::ComPtr<ID3D11SamplerState> samplerState)
{
	// Look for the variable and verify
	const SimpleSampler* sampInfo = GetSamplerInfo(name);
	if (sampInfo == 0)
	{
		if (ReportWarnings)
		{
			LogWarning("SimpleDomainShader::SetSamplerState() - Sampler named '");
			Log(name);
			LogWarning("' was not found in the shader. Ensure the name is spelled correctly and that it exists in the shader.\n");
		}
		return false;
	}

	// Set the shader resource view
	deviceContext->DSSetSamplers(sampInfo->BindIndex, 1, samplerState.GetAddressOf());

	// Success
	return true;
}



///////////////////////////////////////////////////////////////////////////////
// ------ SIMPLE HULL SHADER --------------------------------------------------
///////////////////////////////////////////////////////////////////////////////

// --------------------------------------------------------
// Constructor just calls the base
// --------------------------------------------------------
SimpleHullShader::SimpleHullShader(Microsoft::WRL::ComPtr<ID3D11Device> device, Microsoft::WRL::ComPtr<ID3D11DeviceContext> context, LPCWSTR shaderFile)
	: ISimpleShader(device, context)
{
	// Load the actual compiled shader file
	this->LoadShaderFile(shaderFile);
}

// --------------------------------------------------------
// Destructor - Clean up actual shader (base will be called automatically)
// --------------------------------------------------------
SimpleHullShader::~SimpleHullShader()
{
	CleanUp();
}

// --------------------------------------------------------
// Handles cleaning up shader and base class clean up
// --------------------------------------------------------
void SimpleHullShader::CleanUp()
{
	ISimpleShader::CleanUp();
}

// --------------------------------------------------------
// Creates the  Direct3D hull shader
//
// shaderBlob - The shader's compiled code
//
// Returns true if shader is created correctly, false otherwise
// --------------------------------------------------------
bool SimpleHullShader::CreateShader(Microsoft::WRL::ComPtr<ID3DBlob> shaderBlob)
{
	// Clean up first, in the event this method is
	// called more than once on the same object
	this->CleanUp();

	// Create the shader from the blob
	HRESULT result = device->CreateHullShader(
		shaderBlob->GetBufferPointer(),
		shaderBlob->GetBufferSize(),
		0,
		shader.GetAddressOf());

	// Check the result
	return (result == S_OK);
}

// --------------------------------------------------------
// Sets the hull shader and constant buffers for
// future  Direct3D drawing
// --------------------------------------------------------
void SimpleHullShader::SetShaderAndCBs()
{
	// Is shader valid?
	if (!shaderValid) return;

	// Set the shader
	deviceContext->HSSetShader(shader.Get(), 0, 0);

	// Set the constant buffers?
	for (unsigned int i = 0; i < constantBufferCount; i++)
	{
		// Skip "buffers" that aren't true constant buffers
		if (constantBuffers[i].Type != D3D11_CT_CBUFFER)
			continue;

		// This is a real constant buffer, so set it
		deviceContext->HSSetConstantBuffers(
			constantBuffers[i].BindIndex,
			1,
			constantBuffers[i].ConstantBuffer.GetAddressOf());
	}
}

// --------------------------------------------------------
// Sets a shader resource view in the hull shader stage
//
// name - The name of the texture resource in the shader
// srv - The shader resource view of the texture in GPU memory
//
// Returns true if a texture of the given name was found, false otherwise
// --------------------------------------------------------
bool SimpleHullShader::SetShaderResourceView(std::string name, Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> srv)
{
	// Look for the variable and verify
	const SimpleSRV* srvInfo = GetShaderResourceViewInfo(name);
	if (srvInfo == 0)
	{
		if (ReportWarnings)
		{
			LogWarning("SimpleHullShader::SetShaderResourceView() - SRV named '");
			Log(name);
			LogWarning("' was not found in the shader. Ensure the name is spelled correctly and that it exists in the shader.\n");
		}
		return false;
	}

	// Set the shader resource view
	deviceContext->HSSetShaderResources(srvInfo->BindIndex, 1, srv.GetAddressOf());

	// Success
	return true;
}

// --------------------------------------------------------
// Sets a sampler state in the hull shader stage
//
// name - The name of the sampler state in the shader
// samplerState - The sampler state in GPU memory
//
// Returns true if a sampler of the given name was found, false otherwise
// --------------------------------------------------------
bool SimpleHullShader::SetSamplerState(std::string name, Microsoft::WRL::ComPtr<ID3D11SamplerState> samplerState)
{
	// Look for the variable and verify
	const SimpleSampler* sampInfo = GetSamplerInfo(name);
	if (sampInfo == 0)
	{
		if (ReportWarnings)
		{
			LogWarning("SimpleHullShader::SetSamplerState() - Sampler named '");
			Log(name);
			LogWarning("' was not found in the shader. Ensure the name is spelled correctly and that it exists in the shader.\n");
		}
		return false;
	}

	// Set the shader resource view
	deviceContext->HSSetSamplers(sampInfo->BindIndex, 1, samplerState.GetAddressOf());

	// Success
	return true;
}




///////////////////////////////////////////////////////////////////////////////
// ------ SIMPLE GEOMETRY SHADER ----------------------------------------------
///////////////////////////////////////////////////////////////////////////////

// --------------------------------------------------------
// Constructor calls the base and sets up potential stream-out options
// --------------------------------------------------------
SimpleGeometryShader::SimpleGeometryShader(Microsoft::WRL::ComPtr<ID3D11Device> device, Microsoft::WRL::ComPtr<ID3D11DeviceContext> context, LPCWSTR shaderFile, bool useStreamOut, bool allowStreamOutRasterization)
	: ISimpleShader(device, context)
{
	this->streamOutVertexSize = 0;
	this->useStreamOut = useStreamOut;
	this->allowStreamOutRasterization = allowStreamOutRasterization;

	// Load the actual compiled shader file
	this->LoadShaderFile(shaderFile);
}

// --------------------------------------------------------
// Destructor - Clean up actual shader (base will be called automatically)
// --------------------------------------------------------
SimpleGeometryShader::~SimpleGeometryShader()
{
	CleanUp();
}

// --------------------------------------------------------
// Handles cleaning up shader and base class clean up
// --------------------------------------------------------
void SimpleGeometryShader::CleanUp()
{
	ISimpleShader::CleanUp();
}

// --------------------------------------------------------
// Creates the  Direct3D Geometry shader
//
// shaderBlob - The shader's compiled code
//
// Returns true if shader is created correctly, false otherwise
// --------------------------------------------------------
bool SimpleGeometryShader::CreateShader(Microsoft::WRL::ComPtr<ID3DBlob> shaderBlob)
{
	// Clean up first, in the event this method is
	// called more than once on the same object
	this->CleanUp();

	// Using stream out?
	if (useStreamOut)
		return this->CreateShaderWithStreamOut(shaderBlob);

	// Create the shader from the blob
	HRESULT result = device->CreateGeometryShader(
		shaderBlob->GetBufferPointer(),
		shaderBlob->GetBufferSize(),
		0,
		shader.GetAddressOf());

	// Check the result
	return (result == S_OK);
}

// --------------------------------------------------------
// Creates the  Direct3D Geometry shader and sets it up for
// stream output, if possible.
//
// shaderBlob - The shader's compiled code
//
// Returns true if shader is created correctly, false otherwise
// --------------------------------------------------------
bool SimpleGeometryShader::CreateShaderWithStreamOut(Microsoft::WRL::ComPtr<ID3DBlob> shaderBlob)
{
	// Clean up first, in the event this method is
	// called more than once on the same object
	this->CleanUp();

	// Reflect shader info
	Microsoft::WRL::ComPtr<ID3D11ShaderReflection> refl;
	D3DReflect(
		shaderBlob->GetBufferPointer(),
		shaderBlob->GetBufferSize(),
		IID_ID3D11ShaderReflection,
		(void**)refl.GetAddressOf());

	// Get shader info
	D3D11_SHADER_DESC shaderDesc;
	refl->GetDesc(&shaderDesc);

	// Set up the output signature
	streamOutVertexSize = 0;
	std::vector<D3D11_SO_DECLARATION_ENTRY> soDecl;
	for (unsigned int i = 0; i < shaderDesc.OutputParameters; i++)
	{
		// Get the info about this entry
		D3D11_SIGNATURE_PARAMETER_DESC paramDesc;
		refl->GetOutputParameterDesc(i, &paramDesc);

		// Create the SO Declaration
		D3D11_SO_DECLARATION_ENTRY entry = {};
		entry.SemanticIndex = paramDesc.SemanticIndex;
		entry.SemanticName = paramDesc.SemanticName;
		entry.Stream = paramDesc.Stream;
		entry.StartComponent = 0; // Assume starting at 0
		entry.OutputSlot = 0; // Assume the first output slot

		// Check the mask to determine how many components are used
		entry.ComponentCount = CalcComponentCount(paramDesc.Mask);

		// Increment the size
		streamOutVertexSize += entry.ComponentCount * sizeof(float);

		// Add to the declaration
		soDecl.push_back(entry);
	}

	// Rasterization allowed?
	unsigned int rast = allowStreamOutRasterization ? 0 : D3D11_SO_NO_RASTERIZED_STREAM;

	// Create the shader
	HRESULT result = device->CreateGeometryShaderWithStreamOutput(
		shaderBlob->GetBufferPointer(), // Shader blob pointer
		shaderBlob->GetBufferSize(),    // Shader blob size
		&soDecl[0],                     // Stream out declaration
		(unsigned int)soDecl.size(),    // Number of declaration entries
		NULL,                           // Buffer strides (not used - assume tightly packed?)
		0,                              // No buffer strides
		rast,                           // Index of the stream to rasterize (if any)
		NULL,                           // Not using class linkage
		shader.GetAddressOf());

	return (result == S_OK);
}

// --------------------------------------------------------
// Creates a vertex buffer that is compatible with the stream output
// delcaration that was used to create the shader.  This buffer will
// not be cleaned up (Released) by the simple shader - you must clean
// it up yourself when you're done with it.  Immediately returns
// false if the shader was not created with stream output, the shader
// isn't valid or the determined stream out vertex size is zero.
//
// buffer - Pointer to an ID3D11Buffer pointer to hold the buffer ref
// vertexCount - Amount of vertices the buffer should hold
//
// Returns true if buffer is created successfully AND stream output
// was used to create the shader.  False otherwise.
// --------------------------------------------------------
bool SimpleGeometryShader::CreateCompatibleStreamOutBuffer(Microsoft::WRL::ComPtr<ID3D11Buffer> buffer, int vertexCount)
{
	// Was stream output actually used?
	if (!this->useStreamOut || !shaderValid || streamOutVertexSize == 0)
	{
		if (ReportErrors)
		{
			LogError("SimpleGeometryShader::CreateCompatibleStreamOutBuffer() - Either the shader is not valid or this SimpleGeometryShader was not initialized for stream out usage.\n");
		}

		return false;
	}

	// Set up the buffer description
	D3D11_BUFFER_DESC desc = {};
	desc.BindFlags = D3D11_BIND_STREAM_OUTPUT | D3D11_BIND_VERTEX_BUFFER;
	desc.ByteWidth = streamOutVertexSize * vertexCount;
	desc.CPUAccessFlags = 0;
	desc.MiscFlags = 0;
	desc.StructureByteStride = 0;
	desc.Usage = D3D11_USAGE_DEFAULT;

	// Attempt to create the buffer and return the result
	HRESULT result = device->CreateBuffer(&desc, 0, buffer.GetAddressOf());
	return (result == S_OK);
}

// --------------------------------------------------------
// Helper method to unbind all stream out buffers from the SO stage
// --------------------------------------------------------
void SimpleGeometryShader::UnbindStreamOutStage(Microsoft::WRL::ComPtr<ID3D11DeviceContext> deviceContext)
{
	unsigned int offset = 0;
	ID3D11Buffer* unset[4] = { 0, 0, 0, 0 }; // Max of 4 output targets according to  Direct3D documentation
	deviceContext->SOSetTargets(4, unset, &offset);
}

// --------------------------------------------------------
// Sets the geometry shader and constant buffers for
// future  Direct3D drawing
// --------------------------------------------------------
void SimpleGeometryShader::SetShaderAndCBs()
{
	// Is shader valid?
	if (!shaderValid) return;

	// Set the shader
	deviceContext->GSSetShader(shader.Get(), 0, 0);

	// Set the constant buffers?
	for (unsigned int i = 0; i < constantBufferCount; i++)
	{
		// Skip "buffers" that aren't true constant buffers
		if (constantBuffers[i].Type != D3D11_CT_CBUFFER)
			continue;

		// This is a real constant buffer, so set it
		deviceContext->GSSetConstantBuffers(
			constantBuffers[i].BindIndex,
			1,
			constantBuffers[i].ConstantBuffer.GetAddressOf());
	}
}

// --------------------------------------------------------
// Sets a shader resource view in the Geometry shader stage
//
// name - The name of the texture resource in the shader
// srv - The shader resource view of the texture in GPU memory
//
// Returns true if a texture of the given name was found, false otherwise
// --------------------------------------------------------
bool SimpleGeometryShader::SetShaderResourceView(std::string name, Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> srv)
{
	// Look for the variable and verify
	const SimpleSRV* srvInfo = GetShaderResourceViewInfo(name);
	if (srvInfo == 0)
	{
		if (ReportWarnings)
		{
			LogWarning("SimpleGeometryShader::SetShaderResourceView() - SRV named '");
			Log(name);
			LogWarning("' was not found in the shader. Ensure the name is spelled correctly and that it exists in the shader.\n");
		}
		return false;
	}

	// Set the shader resource view
	deviceContext->GSSetShaderResources(srvInfo->BindIndex, 1, srv.GetAddressOf());

	// Success
	return true;
}

// --------------------------------------------------------
// Sets a sampler state in the Geometry shader stage
//
// name - The name of the sampler state in the shader
// samplerState - The sampler state in GPU memory
//
// Returns true if a sampler of the given name was found, false otherwise
// --------------------------------------------------------
bool SimpleGeometryShader::SetSamplerState(std::string name, Microsoft::WRL::ComPtr<ID3D11SamplerState> samplerState)
{
	// Look for the variable and verify
	const SimpleSampler* sampInfo = GetSamplerInfo(name);
	if (sampInfo == 0)
	{
		if (ReportWarnings)
		{
			LogWarning("SimpleGeometryShader::SetSamplerState() - Sampler named '");
			Log(name);
			LogWarning("' was not found in the shader. Ensure the name is spelled correctly and that it exists in the shader.\n");
		}
		return false;
	}

	// Set the shader resource view
	deviceContext->GSSetSamplers(sampInfo->BindIndex, 1, samplerState.GetAddressOf());

	// Success
	return true;
}

// --------------------------------------------------------
// Calculates the number of components specified by a parameter description mask
//
// mask - The mask to check (only values 0 - 15 are considered)
//
// Returns an integer between 0 - 4 inclusive
// --------------------------------------------------------
unsigned int SimpleGeometryShader::CalcComponentCount(unsigned int mask)
{
	unsigned int result = 0;
	result += (unsigned int)((mask & 1) == 1);
	result += (unsigned int)((mask & 2) == 2);
	result += (unsigned int)((mask & 4) == 4);
	result += (unsigned int)((mask & 8) == 8);
	return result;
}



///////////////////////////////////////////////////////////////////////////////
// ------ SIMPLE COMPUTE SHADER -----------------------------------------------
///////////////////////////////////////////////////////////////////////////////

// --------------------------------------------------------
// Constructor just calls the base
// --------------------------------------------------------
SimpleComputeShader::SimpleComputeShader(Microsoft::WRL::ComPtr<ID3D11Device> device, Microsoft::WRL::ComPtr<ID3D11DeviceContext> context, LPCWSTR shaderFile)
	: ISimpleShader(device, context)
{
	this->threadsTotal = 0;
	this->threadsX = 0;
	this->threadsY = 0;
	this->threadsZ = 0;

	// Load the actual compiled shader file
	this->LoadShaderFile(shaderFile);
}

// --------------------------------------------------------
// Destructor - Clean up actual shader (base will be called automatically)
// --------------------------------------------------------
SimpleComputeShader::~SimpleComputeShader()
{
	CleanUp();
}

// --------------------------------------------------------
// Handles cleaning up shader and base class clean up
// --------------------------------------------------------
void SimpleComputeShader::CleanUp()
{
	ISimpleShader::CleanUp();

	uavTable.clear();
}

// --------------------------------------------------------
// Creates the  Direct3D Compute shader
//
// shaderBlob - The shader's compiled code
//
// Returns true if shader is created correctly, false otherwise
// --------------------------------------------------------
bool SimpleComputeShader::CreateShader(Microsoft::WRL::ComPtr<ID3DBlob> shaderBlob)
{
	// Clean up first, in the event this method is
	// called more than once on the same object
	this->CleanUp();

	// Create the shader from the blob
	HRESULT result = device->CreateComputeShader(
		shaderBlob->GetBufferPointer(),
		shaderBlob->GetBufferSize(),
		0,
		shader.GetAddressOf());

	// Was the shader created correctly?
	if (result != S_OK)
		return false;

	// Set up shader reflection to get information about UAV's
	Microsoft::WRL::ComPtr<ID3D11ShaderReflection> refl;
	D3DReflect(
		shaderBlob->GetBufferPointer(),
		shaderBlob->GetBufferSize(),
		IID_ID3D11ShaderReflection,
		(void**)refl.GetAddressOf());

	// Get the description of the shader
	D3D11_SHADER_DESC shaderDesc;
	refl->GetDesc(&shaderDesc);

	// Grab the thread info
	threadsTotal = refl->GetThreadGroupSize(
		&threadsX,
		&threadsY,
		&threadsZ);

	// Loop and get all UAV resources
	unsigned int resourceCount = shaderDesc.BoundResources;
	for (unsigned int r = 0; r < resourceCount; r++)
	{
		// Get this resource's description
		D3D11_SHADER_INPUT_BIND_DESC resourceDesc;
		refl->GetResourceBindingDesc(r, &resourceDesc);

		// Check the type, looking for any kind of UAV
		switch (resourceDesc.Type)
		{
		case D3D_SIT_UAV_APPEND_STRUCTURED:
		case D3D_SIT_UAV_CONSUME_STRUCTURED:
		case D3D_SIT_UAV_RWBYTEADDRESS:
		case D3D_SIT_UAV_RWSTRUCTURED:
		case D3D_SIT_UAV_RWSTRUCTURED_WITH_COUNTER:
		case D3D_SIT_UAV_RWTYPED:
			uavTable.insert(std::pair<std::string, unsigned int>(resourceDesc.Name, resourceDesc.BindPoint));
		}
	}

	// All set
	return true;
}

// --------------------------------------------------------
// Sets the Compute shader and constant buffers for
// future  Direct3D drawing
// --------------------------------------------------------
void SimpleComputeShader::SetShaderAndCBs()
{
	// Is shader valid?
	if (!shaderValid) return;

	// Set the shader
	deviceContext->CSSetShader(shader.Get(), 0, 0);

	// Set the constant buffers?
	for (unsigned int i = 0; i < constantBufferCount; i++)
	{
		// Skip "buffers" that aren't true constant buffers
		if (constantBuffers[i].Type != D3D11_CT_CBUFFER)
			continue;

		// This is a real constant buffer, so set it
		deviceContext->CSSetConstantBuffers(
			constantBuffers[i].BindIndex,
			1,
			constantBuffers[i].ConstantBuffer.GetAddressOf());
	}
}

// --------------------------------------------------------
// Dispatches the compute shader with the specified amount 
// of groups, using the number of threads per group
// specified in the shader file itself
//
// For example, calling this method with params (5,1,1) on
// a shader with (8,2,2) threads per group will launch a 
// total of 160 threads: ((5 * 8) * (1 * 2) * (1 * 2))
//
// This is identical to using the device context's 
// Dispatch() method yourself.  
//
// Note: This will dispatch the currently active shader, 
// not necessarily THIS shader. Be sure to activate this
// shader with SetShader() before calling Dispatch
//
// groupsX - Numbers of groups in the X dimension
// groupsY - Numbers of groups in the Y dimension
// groupsZ - Numbers of groups in the Z dimension
// --------------------------------------------------------
void SimpleComputeShader::DispatchByGroups(unsigned int groupsX, unsigned int groupsY, unsigned int groupsZ)
{
	deviceContext->Dispatch(groupsX, groupsY, groupsZ);
}

// --------------------------------------------------------
// Dispatches the compute shader with AT LEAST the 
// specified amount of threads, calculating the number of
// groups to dispatch using the number of threads per group
// specified in the shader file itself
//
// For example, calling this method with params (10,3,3) on
// a shader with (5,2,2) threads per group will launch 
// 8 total groups and 160 total threads, calculated by:
// Groups: ceil(10/5) * ceil(3/2) * ceil(3/2) = 8
// Threads: ((2 * 5) * (2 * 2) * (2 * 2)) = 160
//
// Note: This will dispatch the currently active shader, 
// not necessarily THIS shader. Be sure to activate this
// shader with SetShader() before calling Dispatch
//
// threadsX - Desired numbers of threads in the X dimension
// threadsY - Desired numbers of threads in the Y dimension
// threadsZ - Desired numbers of threads in the Z dimension
// --------------------------------------------------------
void SimpleComputeShader::DispatchByThreads(unsigned int threadsX, unsigned int threadsY, unsigned int threadsZ)
{
	deviceContext->Dispatch(
		max((unsigned int)ceil((float)threadsX / this->threadsX), 1),
		max((unsigned int)ceil((float)threadsY / this->threadsY), 1),
		max((unsigned int)ceil((float)threadsZ / this->threadsZ), 1));
}

// --------------------------------------------------------
// Determines if this shader has the specified UAV
// --------------------------------------------------------
bool SimpleComputeShader::HasUnorderedAccessView(std::string name)
{
	return GetUnorderedAccessViewIndex(name) != -1;
}

// --------------------------------------------------------
// Sets a shader resource view in the Compute shader stage
//
// name - The name of the texture resource in the shader
// srv - The shader resource view of the texture in GPU memory
//
// Returns true if a texture of the given name was found, false otherwise
// --------------------------------------------------------
bool SimpleComputeShader::SetShaderResourceView(std::string name, Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> srv)
{
	// Look for the variable and verify
	const SimpleSRV* srvInfo = GetShaderResourceViewInfo(name);
	if (srvInfo == 0)
	{
		if (ReportWarnings)
		{
			LogWarning("SimpleComputeShader::SetShaderResourceView() - SRV named '");
			Log(name);
			LogWarning("' was not found in the shader. Ensure the name is spelled correctly and that it exists in the shader.\n");
		}
		return false;
	}

	// Set the shader resource view
	deviceContext->CSSetShaderResources(srvInfo->BindIndex, 1, srv.GetAddressOf());

	// Success
	return true;
}

// --------------------------------------------------------
// Sets a sampler state in the Compute shader stage
//
// name - The name of the sampler state in the shader
// samplerState - The sampler state in GPU memory
//
// Returns true if a sampler of the given name was found, false otherwise
// --------------------------------------------------------
bool SimpleComputeShader::SetSamplerState(std::string name, Microsoft::WRL::ComPtr<ID3D11SamplerState> samplerState)
{
	// Look for the variable and verify
	const SimpleSampler* sampInfo = GetSamplerInfo(name);
	if (sampInfo == 0)
	{
		if (ReportWarnings)
		{
			LogWarning("SimpleComputeShader::SetSamplerState() - Sampler named '");
			Log(name);
			LogWarning("' was not found in the shader. Ensure the name is spelled correctly and that it exists in the shader.\n");
		}
		return false;
	}

	// Set the shader resource view
	deviceContext->CSSetSamplers(sampInfo->BindIndex, 1, samplerState.GetAddressOf());

	// Success
	return true;
}

// --------------------------------------------------------
// Sets an unordered access view in the Compute shader stage
//
// name - The name of the sampler state in the shader
// uav - The UAV in GPU memory
// appendConsumeOffset - Used for append or consume UAV's (optional)
//
// Returns true if a UAV of the given name was found, false otherwise
// --------------------------------------------------------
bool SimpleComputeShader::SetUnorderedAccessView(std::string name, Microsoft::WRL::ComPtr<ID3D11UnorderedAccessView> uav, unsigned int appendConsumeOffset)
{
	// Look for the variable and verify
	unsigned int bindIndex = GetUnorderedAccessViewIndex(name);
	if (bindIndex == -1)
	{
		if (ReportWarnings)
		{
			LogWarning("SimpleComputeShader::SetUnorderedAccessView() - UAV named '");
			Log(name);
			LogWarning("' was not found in the shader. Ensure the name is spelled correctly and that it exists in the shader.\n");
		}
		return false;
	}

	// Set the shader resource view
	deviceContext->CSSetUnorderedAccessViews(bindIndex, 1, uav.GetAddressOf(), &appendConsumeOffset);

	// Success
	return true;
}

// --------------------------------------------------------
// Gets the index of the specified UAV (or -1)
// --------------------------------------------------------
int SimpleComputeShader::GetUnorderedAccessViewIndex(std::string name)
{
	// Look for the key
	std::unordered_map<std::string, unsigned int>::iterator result =
		uavTable.find(name);

	// Did we find the key?
	if (result == uavTable.end())
		return -1;

	// Success
	return result->second;
}