{ demo.pas : main program }
program demo;

uses greetunit, mathunit;

var
  I: Integer;
begin
  Greet('World');
  WriteLn('5! = ', Factorial(5));
  Write('Primes under 20:');
  for I := 2 to 19 do
    if IsPrime(I) then
      Write(' ', I);
  WriteLn;
  Farewell('World');
end.
