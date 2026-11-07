import java.util.*;
public class SelfMap { public static void main(String[] a){ Map<Object,Object> m=new HashMap<>(); m.put(1.0, m);
  try { System.out.println(String.format("%s: %08x", "table", m.hashCode())); } catch (StackOverflowError e) { System.out.println("StackOverflowError from getStringCoerced-style hashCode on a cyclic converted table"); } } }
