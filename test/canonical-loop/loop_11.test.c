void foo() {
  #pragma sapfor analysis dependency(<I,4>) explicitaccess(<I,4>)
  #pragma sapfor analysis perfect
  #pragma sapfor analysis non-canonical
  for (int I; I < 10; I++);
}