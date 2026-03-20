{ mathunit.pas : simple math utilities }
unit mathunit;

interface

function Factorial(N: Integer): Int64;
function IsPrime(N: Integer): Boolean;

implementation

function Factorial(N: Integer): Int64;
var
  I: Integer;
  Result_: Int64;
begin
  Result_ := 1;
  for I := 2 to N do
    Result_ := Result_ * I;
  Factorial := Result_;
end;

function IsPrime(N: Integer): Boolean;
var
  I: Integer;
begin
  if N < 2 then begin
    IsPrime := False;
    Exit;
  end;
  I := 2;
  while I * I <= N do begin
    if N mod I = 0 then begin
      IsPrime := False;
      Exit;
    end;
    Inc(I);
  end;
  IsPrime := True;
end;

end.
